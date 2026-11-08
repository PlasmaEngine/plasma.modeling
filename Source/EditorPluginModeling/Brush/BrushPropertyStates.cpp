#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushPropertyStates.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>

namespace
{
  void BrushPropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
  {
    static const plRTTI* pBrushType = plRTTI::FindTypeByName("plBrushComponent");

    if (pBrushType == nullptr || e.m_pObject->GetTypeAccessor().GetType() != pBrushType)
      return;

    auto& props = *e.m_pPropertyStates;
    const auto& acc = e.m_pObject->GetTypeAccessor();

    const plInt64 iShape = acc.GetValue("Shape").ConvertTo<plInt64>();
    const bool bSubtractive = acc.GetValue("Subtractive").ConvertTo<bool>();

    const auto hide = [&](const char* szProperty)
    { props[szProperty].m_Visibility = plPropertyUiState::Invisible; };

    // generator parameters, per shape
    bool bDetail = false, bCurvature = false, bThickness = false, bSize = true;

    switch (iShape)
    {
      case plBrushShape::Box:
        bThickness = true; // Hollow's wall thickness
        break;
      case plBrushShape::Column:
      case plBrushShape::Cone:
        bDetail = true;
        props["Detail"].m_sNewLabelText = "Sides";
        break;
      case plBrushShape::Sphere:
        bDetail = true;
        props["Detail"].m_sNewLabelText = "Segments";
        break;
      case plBrushShape::Pyramid:
        break;
      case plBrushShape::StairsX:
      case plBrushShape::StairsY:
        bDetail = true;
        bCurvature = true;
        props["Detail"].m_sNewLabelText = "Steps";
        break;
      case plBrushShape::ArchX:
      case plBrushShape::ArchY:
      case plBrushShape::SpiralStairs:
        bDetail = true;
        bCurvature = true;
        bThickness = true;
        props["Detail"].m_sNewLabelText = "Segments";
        props["Curvature"].m_sNewLabelText = "Sweep";
        break;
      case plBrushShape::Editable:
        // the stored mesh decides the shape; Thickness remains for Hollow
        bSize = false;
        bThickness = true;
        break;
      default:
        break;
    }

    if (!bSize)
    {
      for (const char* szSize : {"SizeNegX", "SizePosX", "SizeNegY", "SizePosY", "SizeNegZ", "SizePosZ"})
        hide(szSize);
    }

    if (!bDetail)
      hide("Detail");
    if (!bCurvature)
      hide("Curvature");
    if (!bThickness)
      hide("Thickness");

    // a cutter never renders in a game or collides: its look and output settings do nothing
    if (bSubtractive)
    {
      for (const char* szProperty : {"Materials", "Color", "CustomData", "SmoothingAngle", "TextureLock", "GenerateCollision", "IncludeInNavmesh", "UseAsOccluder", "MergeOnExport"})
        hide(szProperty);
    }
  }
} // namespace

void plBrushPropertyStates::Startup()
{
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(BrushPropertyMetaStateEventHandler);
}

void plBrushPropertyStates::Shutdown()
{
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(BrushPropertyMetaStateEventHandler);
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushPropertyStates);
