#include <EditorPluginModelingMcp/EditorPluginModelingMcpPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushCommands.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/BrushExport.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ModelingPlugin/Modeling/MeshOps.h>
#include <ModelingPlugin/Modeling/MeshTopology.h>
#include <ModelingPlugin/Modeling/MeshValidator.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

/// \brief MCP tools for the modeling package: frame the view, create and inspect brushes, and run every
/// modeling operation. They call the same commands as the panel and the keys.
class plMcpModelingTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpModelingTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void View(const plVariantDictionary& args, plMcpToolResult& out_result);
  void CreateBrush(const plVariantDictionary& args, plMcpToolResult& out_result);
  void BrushInfo(const plVariantDictionary& args, plMcpToolResult& out_result);
  void SetFaceMaterial(const plVariantDictionary& args, plMcpToolResult& out_result);
  void FaceTexture(const plVariantDictionary& args, plMcpToolResult& out_result);
  void Csg(const plVariantDictionary& args, plMcpToolResult& out_result);
  void MeshOp(const plVariantDictionary& args, plMcpToolResult& out_result);
  void Tool(const plVariantDictionary& args, plMcpToolResult& out_result);
  void Export(const plVariantDictionary& args, plMcpToolResult& out_result);
};

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpModelingTool, 1, plRTTIDefaultAllocator<plMcpModelingTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  const plVariantArray* GetArray(const plVariantDictionary& args, plStringView sKey)
  {
    const plVariant* pValue = args.GetValue(sKey);
    return (pValue && pValue->IsA<plVariantArray>()) ? &pValue->Get<plVariantArray>() : nullptr;
  }

  bool GetVec3(const plVariantDictionary& args, plStringView sKey, plVec3& out_v)
  {
    const plVariantArray* pArray = GetArray(args, sKey);
    if (pArray == nullptr || pArray->GetCount() != 3)
      return false;

    out_v.Set((*pArray)[0].ConvertTo<float>(), (*pArray)[1].ConvertTo<float>(), (*pArray)[2].ConvertTo<float>());
    return true;
  }

  double GetNumber(const plVariantDictionary& args, plStringView sKey, double fFallback)
  {
    const plVariant* pValue = args.GetValue(sKey);
    return (pValue && pValue->CanConvertTo<double>()) ? pValue->ConvertTo<double>() : fFallback;
  }

  void GetUIntSet(const plVariantDictionary& args, plStringView sKey, plHashSet<plUInt32>& out_set)
  {
    out_set.Clear();
    if (const plVariantArray* pArray = GetArray(args, sKey))
    {
      for (const plVariant& v : *pArray)
        out_set.Insert(v.ConvertTo<plUInt32>());
    }
  }

  /// edges as [[v0, v1], ...]
  void GetEdges(const plVariantDictionary& args, plStringView sKey, plHashSet<plUInt64>& out_edges)
  {
    out_edges.Clear();
    if (const plVariantArray* pArray = GetArray(args, sKey))
    {
      for (const plVariant& v : *pArray)
      {
        if (v.IsA<plVariantArray>() && v.Get<plVariantArray>().GetCount() == 2)
        {
          const auto& pair = v.Get<plVariantArray>();
          out_edges.Insert(plMeshTopology::MakeKey(pair[0].ConvertTo<plUInt32>(), pair[1].ConvertTo<plUInt32>()));
        }
      }
    }
  }

  /// A brush component from the guid of the component or of its game object.
  const plDocumentObject* ResolveBrush(plDocument* pDocument, plStringView sGuid)
  {
    plUuid guid;
    if (!plConversionUtils::TryConvertStringToUuid(sGuid, guid))
      return nullptr;

    const plDocumentObject* pObject = pDocument->GetObjectManager()->GetObject(guid);
    if (pObject == nullptr)
      return nullptr;

    if (plBrushPicking::IsBrushComponent(pObject))
      return pObject;

    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (plBrushPicking::IsBrushComponent(pChild))
        return pChild;
    }

    return nullptr;
  }

  void ResolveBrushes(plDocument* pDocument, const plVariantDictionary& args, plDynamicArray<const plDocumentObject*>& out_brushes)
  {
    out_brushes.Clear();
    if (const plVariantArray* pArray = GetArray(args, "brushes"))
    {
      for (const plVariant& v : *pArray)
      {
        if (const plDocumentObject* pBrush = ResolveBrush(pDocument, v.ConvertTo<plString>()))
          out_brushes.PushBack(pBrush);
      }
    }
  }

  plString GuidString(const plUuid& guid)
  {
    plStringBuilder s;
    plConversionUtils::ToString(guid, s);
    return s;
  }

  void Finish(plMcpToolResult& out_result, const plStatus& status)
  {
    if (status.Failed())
    {
      out_result.SetError(status.m_sMessage.IsEmpty() ? plStringView("The operation failed.") : status.m_sMessage.GetView());
      return;
    }

    plMcpJsonWriter writer;
    writer.BeginObject();
    writer.AddVariableBool("success", true);
    if (!status.m_sMessage.IsEmpty())
      writer.AddVariableString("message", status.m_sMessage);
    writer.EndObject();
    out_result.m_sText = writer.GetResult();
  }

  plBrushToolContext* GetBrushContext(plDocument* pDocument, bool bActivate)
  {
    plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(pDocument);
    if (pGameDoc == nullptr)
      return nullptr;

    if (bActivate && (!pGameDoc->GetActiveEditTool() || !pGameDoc->GetActiveEditTool()->IsInstanceOf<plBrushEditTool>()))
      pGameDoc->SetActiveEditTool(plGetStaticRTTI<plBrushEditTool>());

    plBrushEditTool* pTool = plDynamicCast<plBrushEditTool*>(pGameDoc->GetActiveEditTool());
    return pTool ? &pTool->GetToolContext() : nullptr;
  }

} // namespace

// schema fragments shared by the tools (literal concatenation: the formatter does not escape braces)
#define PL_MODELING_DOC R"("document":{"type":"string","description":"Guid or path of the open scene."})"
#define PL_MODELING_BRUSH R"("brush":{"type":"string","description":"Guid of the brush component or of its game object."})"
#define PL_MODELING_NUMBERS R"({"type":"array","items":{"type":"number"}})"
#define PL_MODELING_STRINGS R"({"type":"array","items":{"type":"string"}})"

void plMcpModelingTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_view";
    d.m_sDescription = "Points every perspective viewport of a scene window at a target: from 'eye' toward 'target' (world metres, Z up). Use it to frame brushes before a screenshot. Changes only the editor camera, not the scene.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC R"(,"eye":)" PL_MODELING_NUMBERS R"(,"target":)" PL_MODELING_NUMBERS R"(},"required":["document","eye","target"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_create_brush";
    d.m_sDescription = "Adds a brush (level geometry that renders, collides and bakes into the navmesh) as a new object. Shapes: Box, RampX, RampY, Column, StairsX, StairsY, ArchX, ArchY, SpiralStairs, Sphere, Cone, Pyramid. 'size' is [negX, posX, negY, posY, negZ, posZ] extents from the object origin in metres. 'detail' = sides/steps/segments, 'curvature' = sweep in degrees. 'subtractive' makes it a cutter that carves overlapping brushes. 'parent' puts it under an object (e.g. a brush group). Returns the component and object guids. One undo step.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC R"(,"shape":{"type":"string"},"name":{"type":"string"},"position":)" PL_MODELING_NUMBERS R"(,"size":)" PL_MODELING_NUMBERS
      R"(,"detail":{"type":"number"},"curvature":{"type":"number"},"thickness":{"type":"number"},"subtractive":{"type":"boolean"},"material":{"type":"string","description":"Material asset guid"},"parent":{"type":"string"}},"required":["document"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_brush_info";
    d.m_sDescription = "Describes a brush: its authored (uncut) mesh with vertex positions and faces (id, corner vertex indices, centre, normal) - the indices modeling_mesh_op and modeling_tool take - the rendered mesh's face count and cut face ids, and a health report (bent faces, open edges, convexity). Reads only.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC "," PL_MODELING_BRUSH R"(,"includeVertices":{"type":"boolean"}},"required":["document","brush"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_set_face_material";
    d.m_sDescription = "Assigns a material asset (guid) to faces of a brush by face id (from modeling_brush_info; cut faces included), or to the whole brush when 'faceIds' is omitted. One undo step.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC "," PL_MODELING_BRUSH R"(,"material":{"type":"string"},"faceIds":)" PL_MODELING_NUMBERS R"(},"required":["document","brush","material"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_face_texture";
    d.m_sDescription = "Sets the texture projection of brush faces: 'alignment' world|face, 'tileSize' [u, v] metres per repeat, 'offset' [u, v] in repeats, 'rotation' degrees, 'hardEdges', or 'justify' left|right|top|bottom|center|fit. Unset fields keep their value. One undo step.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC "," PL_MODELING_BRUSH R"(,"faceIds":)" PL_MODELING_NUMBERS R"(,"alignment":{"type":"string"},"tileSize":)" PL_MODELING_NUMBERS
      R"(,"offset":)" PL_MODELING_NUMBERS R"(,"rotation":{"type":"number"},"hardEdges":{"type":"boolean"},"justify":{"type":"string"}},"required":["document","brush","faceIds"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_csg";
    d.m_sDescription = "Whole-brush operations on 'brushes' (guids): toggle_cutter (cutters carve live, nothing else is needed), bake_cuts (make the selected cutters' cuts permanent and delete them), hollow (walls of each brush's Thickness), union (merge into the first), decompose (split non-convex cutters into convex pieces), group (wrap in a brush group), clip (cut with 'plane' [nx, ny, nz, d] where n.p + d = 0, 'keep' front|back|both). One undo step.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC R"(,"operation":{"type":"string"},"brushes":)" PL_MODELING_STRINGS R"(,"plane":)" PL_MODELING_NUMBERS R"(,"keep":{"type":"string"}},"required":["document","operation","brushes"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_mesh_op";
    d.m_sDescription = "Polygon operation on a brush's authored mesh: extrude|inset|subdivide|flip|triangulate|flatten|delete|detach|bridge (by 'faceIds'), bevel|loop_cut|fill_hole (by 'edges' [[v0,v1],...]), weld|connect|delete_vertices (by 'vertices'). 'amount' = extrude distance / inset / bevel width (metres), 'segments' = bevel segments or loop cut count, 'position' 0..1 = single loop cut position. Vertex indices change afterwards, face ids stay. One undo step.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC "," PL_MODELING_BRUSH R"(,"operation":{"type":"string"},"faceIds":)" PL_MODELING_NUMBERS R"(,"edges":{"type":"array","items":)" PL_MODELING_NUMBERS
      R"(},"vertices":)" PL_MODELING_NUMBERS R"(,"amount":{"type":"number"},"segments":{"type":"number"},"position":{"type":"number"},"individual":{"type":"boolean"}},"required":["document","brush","operation"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_tool";
    d.m_sDescription = "Drives the Brush tool like a user would: activates it, sets the sub-object 'mode' (object|vertex|edge|face) on the selected brush ('brush' selects it first), sets the selection ('faceIds', 'vertices', 'edges') or runs a 'selectionOp' (all|none|invert|grow|shrink|loop|ring|same_material), or runs a keyboard 'command' by name (e.g. 'Marking Menu', 'Clip'). Returns the tool's mode and selection.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC "," PL_MODELING_BRUSH R"(,"mode":{"type":"string"},"faceIds":)" PL_MODELING_NUMBERS R"(,"vertices":)" PL_MODELING_NUMBERS
      R"(,"edges":{"type":"array","items":)" PL_MODELING_NUMBERS R"(},"selectionOp":{"type":"string"},"command":{"type":"string"}},"required":["document"]})";
  }
  {
    plMcpToolDesc& d = out_tools.ExpandAndGetRef();
    d.m_sName = "modeling_export";
    d.m_sDescription = "Getting geometry out and in: obj (write 'brushes' to an OBJ+MTL, optional 'path'), bake (each brush to Blockout/*.obj + mesh asset; 'replace' swaps the brush for a mesh component), convert_greybox (every Grey Boxing object in the scene becomes a brush), mesh_to_brush ('objects' with mesh components become brushes). Returns what was written.";
    d.m_sInputSchema = R"({"type":"object","properties":{)" PL_MODELING_DOC R"(,"operation":{"type":"string"},"brushes":)" PL_MODELING_STRINGS R"(,"objects":)" PL_MODELING_STRINGS R"(,"path":{"type":"string"},"replace":{"type":"boolean"}},"required":["document","operation"]})";
  }
}

void plMcpModelingTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "modeling_view")
    View(arguments, out_result);
  else if (sToolName == "modeling_create_brush")
    CreateBrush(arguments, out_result);
  else if (sToolName == "modeling_brush_info")
    BrushInfo(arguments, out_result);
  else if (sToolName == "modeling_set_face_material")
    SetFaceMaterial(arguments, out_result);
  else if (sToolName == "modeling_face_texture")
    FaceTexture(arguments, out_result);
  else if (sToolName == "modeling_csg")
    Csg(arguments, out_result);
  else if (sToolName == "modeling_mesh_op")
    MeshOp(arguments, out_result);
  else if (sToolName == "modeling_tool")
    Tool(arguments, out_result);
  else if (sToolName == "modeling_export")
    Export(arguments, out_result);
}

void plMcpModelingTool::View(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  plVec3 vEye, vTarget;

  if (pDocument == nullptr || !GetVec3(args, "eye", vEye) || !GetVec3(args, "target", vTarget))
  {
    out_result.SetError("Needs an open scene 'document' and 'eye' / 'target' as [x, y, z].");
    return;
  }

  plEngineDocumentWindow* pWindow = dynamic_cast<plEngineDocumentWindow*>(plDocumentWindow::FindWindowByDocument(pDocument));
  if (pWindow == nullptr)
  {
    out_result.SetError("The scene has no window.");
    return;
  }

  plUInt32 uiViews = 0;
  for (plEngineViewWidget* pView : pWindow->GetViewWidgets())
  {
    if (pView->m_pViewConfig->m_Perspective != plSceneViewPerspective::Perspective)
      continue;

    pView->m_pViewConfig->m_Camera.LookAt(vEye, vTarget, plVec3(0, 0, 1));
    ++uiViews;
  }

  Finish(out_result, plStatus(PL_SUCCESS, uiViews > 0 ? "Camera placed." : "No perspective view to place."));
}

void plMcpModelingTool::CreateBrush(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  if (pDocument == nullptr)
  {
    out_result.SetError("Needs an open scene 'document'.");
    return;
  }

  plBrushCommands::BrushDesc desc;

  plStringBuilder sShape = plMcpJson::GetString(args, "shape", "Box");
  if (!sShape.StartsWith("plBrushShape::"))
    sShape.Prepend("plBrushShape::");

  plInt64 iShape = 0;
  if (!plReflectionUtils::StringToEnumeration(plGetStaticRTTI<plBrushShape>(), sShape, iShape))
  {
    out_result.SetError("Unknown 'shape'. Use Box, RampX, RampY, Column, StairsX, StairsY, ArchX, ArchY, SpiralStairs, Sphere, Cone or Pyramid.");
    return;
  }

  desc.m_Shape = static_cast<plBrushShape::Enum>(iShape);
  desc.m_sName = plMcpJson::GetString(args, "name", "Brush");

  plVec3 vPos = plVec3::MakeZero();
  GetVec3(args, "position", vPos);
  desc.m_GlobalTransform = plTransform(vPos);

  if (const plVariantArray* pSize = GetArray(args, "size"))
  {
    if (pSize->GetCount() == 6)
    {
      desc.m_Params.m_fSizeNegX = (*pSize)[0].ConvertTo<float>();
      desc.m_Params.m_fSizePosX = (*pSize)[1].ConvertTo<float>();
      desc.m_Params.m_fSizeNegY = (*pSize)[2].ConvertTo<float>();
      desc.m_Params.m_fSizePosY = (*pSize)[3].ConvertTo<float>();
      desc.m_Params.m_fSizeNegZ = (*pSize)[4].ConvertTo<float>();
      desc.m_Params.m_fSizePosZ = (*pSize)[5].ConvertTo<float>();
    }
  }

  desc.m_Params.m_uiDetail = static_cast<plUInt32>(plMcpJson::GetInt(args, "detail", 16));
  desc.m_Params.m_Curvature = plAngle::MakeFromDegree(static_cast<float>(GetNumber(args, "curvature", 0.0)));
  desc.m_Params.m_fThickness = static_cast<float>(GetNumber(args, "thickness", 0.5));
  desc.m_bSubtractive = plMcpJson::GetBool(args, "subtractive", false);
  desc.m_sMaterial = plMcpJson::GetString(args, "material");

  const plStringView sParent = plMcpJson::GetString(args, "parent");
  if (!sParent.IsEmpty())
  {
    plUuid parent;
    if (plConversionUtils::TryConvertStringToUuid(sParent, parent))
      desc.m_Parent = parent;
  }

  const plUuid comp = plBrushCommands::CreateBrush(pDocument, desc);
  if (!comp.IsValid())
  {
    out_result.SetError("The brush could not be created.");
    return;
  }

  const plDocumentObject* pComp = pDocument->GetObjectManager()->GetObject(comp);

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("component", GuidString(comp));
  writer.AddVariableString("object", pComp ? GuidString(pComp->GetParent()->GetGuid()) : plString());
  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpModelingTool::BrushInfo(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  const plDocumentObject* pBrush = pDocument ? ResolveBrush(pDocument, plMcpJson::GetString(args, "brush")) : nullptr;

  if (pBrush == nullptr)
  {
    out_result.SetError("Needs an open scene 'document' and a 'brush' guid.");
    return;
  }

  plEditableMesh base, evaluated;
  const bool bBaseOk = plBrushPicking::BuildMeshFromObject(pBrush, base).Succeeded();
  plDynamicArray<plUInt32> refused;
  plBrushPicking::BuildEvaluatedMeshFromObject(pBrush, evaluated, &refused).IgnoreResult();

  plMeshValidationReport report;
  // convexity only matters for cutters, and costs faces x corners on large meshes
  plMeshValidator::Validate(base, report, pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>());

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("component", GuidString(pBrush->GetGuid()));
  writer.AddVariableBool("meshDamaged", !bBaseOk);
  writer.AddVariableUInt32("brushId", pBrush->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>());
  writer.AddVariableBool("subtractive", pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>());
  writer.AddVariableUInt32("vertexCount", base.m_Vertices.GetCount());
  writer.AddVariableUInt32("faceCount", base.m_Faces.GetCount());
  writer.AddVariableUInt32("shellCount", base.m_Shells.GetCount());
  writer.AddVariableUInt32("renderedFaceCount", evaluated.m_Faces.GetCount());

  writer.BeginArray("refusedCutters");
  for (plUInt32 id : refused)
    writer.WriteUInt32(id);
  writer.EndArray();

  writer.BeginObject("health");
  writer.AddVariableUInt32("nonPlanarFaces", report.m_uiNonPlanarFaces);
  writer.AddVariableUInt32("degenerateFaces", report.m_uiDegenerateFaces);
  writer.AddVariableUInt32("openEdges", report.m_uiOpenEdges);
  writer.AddVariableUInt32("nonManifoldEdges", report.m_uiNonManifoldEdges);
  writer.AddVariableUInt32("nonConvexShells", report.m_uiNonConvexShells);
  writer.EndObject();

  if (plMcpJson::GetBool(args, "includeVertices", true))
  {
    writer.BeginArray("vertices");
    for (const plVec3& v : base.m_Vertices)
    {
      writer.BeginArray();
      writer.WriteFloat(v.x);
      writer.WriteFloat(v.y);
      writer.WriteFloat(v.z);
      writer.EndArray();
    }
    writer.EndArray();
  }

  writer.BeginArray("faces");
  for (plUInt32 f = 0; f < base.m_Faces.GetCount() && f < 512; ++f)
  {
    const plBrushFace& face = base.m_Faces[f];
    writer.BeginObject();
    writer.AddVariableUInt32("id", face.m_uiFaceId);
    writer.AddVariableUInt32("slot", face.m_uiMaterialSlot);
    writer.BeginArray("corners");
    for (plUInt32 c : face.m_Corners)
      writer.WriteUInt32(c);
    writer.EndArray();
    writer.AddVariableVec3("center", base.ComputeFaceCenter(f));
    writer.AddVariableVec3("normal", base.ComputeFaceNormal(f));
    writer.EndObject();
  }
  writer.EndArray();

  // faces that exist only because of cuts
  writer.BeginArray("cutFaceIds");
  plHashSet<plUInt32> seen;
  for (const plBrushFace& face : evaluated.m_Faces)
  {
    if (plBrushFaceIds::IsDerived(face.m_uiFaceId) && !seen.Insert(face.m_uiFaceId))
      writer.WriteUInt32(face.m_uiFaceId);
  }
  writer.EndArray();

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpModelingTool::SetFaceMaterial(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  const plDocumentObject* pBrush = pDocument ? ResolveBrush(pDocument, plMcpJson::GetString(args, "brush")) : nullptr;
  const plStringView sMaterial = plMcpJson::GetString(args, "material");

  if (pBrush == nullptr || sMaterial.IsEmpty())
  {
    out_result.SetError("Needs an open scene 'document', a 'brush' guid and a 'material' asset guid.");
    return;
  }

  plHashSet<plUInt32> faceIds;
  GetUIntSet(args, "faceIds", faceIds);

  if (faceIds.IsEmpty())
    Finish(out_result, plBrushCommands::SetBrushMaterial(pDocument, pBrush, sMaterial));
  else
    Finish(out_result, plBrushCommands::SetFaceMaterial(pDocument, pBrush, faceIds, sMaterial));
}

void plMcpModelingTool::FaceTexture(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  const plDocumentObject* pBrush = pDocument ? ResolveBrush(pDocument, plMcpJson::GetString(args, "brush")) : nullptr;

  plHashSet<plUInt32> faceIds;
  GetUIntSet(args, "faceIds", faceIds);

  if (pBrush == nullptr || faceIds.IsEmpty())
  {
    out_result.SetError("Needs an open scene 'document', a 'brush' guid and 'faceIds'.");
    return;
  }

  // current mapping and extents per face, from the mesh as rendered
  plEditableMesh mesh;
  plBrushPicking::BuildEvaluatedMeshFromObject(pBrush, mesh).IgnoreResult();

  const plStringView sAlignment = plMcpJson::GetString(args, "alignment");
  const plStringView sJustify = plMcpJson::GetString(args, "justify");
  const plVariantArray* pTile = GetArray(args, "tileSize");
  const plVariantArray* pOffset = GetArray(args, "offset");
  const plVariant* pRotation = args.GetValue("rotation");
  const plVariant* pHard = args.GetValue("hardEdges");

  // the first rendered piece of each face id stands for the face
  plHashTable<plUInt32, plUInt32> faceIndexById;
  for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
  {
    if (!faceIndexById.Contains(mesh.m_Faces[f].m_uiFaceId))
      faceIndexById.Insert(mesh.m_Faces[f].m_uiFaceId, f);
  }

  const plStatus status = plBrushCommands::EditFaceAttributes(pDocument, pBrush, faceIds, "Edit Face Texturing", [&](plUInt32 uiFaceId, plBrushFaceAttributes& attr)
    {
      plUInt32 uiFace = plInvalidIndex;
      if (!faceIndexById.TryGetValue(uiFaceId, uiFace))
        return;

      plBrushFace& face = mesh.m_Faces[uiFace];
      const plVec3 vNormal = mesh.ComputeFaceNormal(uiFace);

      attr.m_uiMask |= plBrushFaceAttributes::UV;
      attr.m_UV = face.m_UV;

      if (sAlignment == "world")
        attr.m_UV.m_bWorldAligned = true;
      else if (sAlignment == "face" && face.m_UV.m_bWorldAligned)
      {
        plVec3 vU = plMath::Abs(vNormal.z) < 0.9f ? plVec3(0, 0, 1).CrossRH(vNormal) : vNormal.GetOrthogonalVector();
        vU.NormalizeIfNotZero(plVec3(1, 0, 0)).IgnoreResult();
        attr.m_UV.m_vUAxis = vU;
        attr.m_UV.m_vVAxis = -vNormal.CrossRH(vU).GetNormalized();
        attr.m_UV.m_bWorldAligned = false;
      }

      if (pTile && pTile->GetCount() == 2)
        attr.m_UV.m_vScale.Set(1.0f / plMath::Max((*pTile)[0].ConvertTo<float>(), 0.0001f), 1.0f / plMath::Max((*pTile)[1].ConvertTo<float>(), 0.0001f));
      if (pOffset && pOffset->GetCount() == 2)
        attr.m_UV.m_vOffset.Set((*pOffset)[0].ConvertTo<float>(), (*pOffset)[1].ConvertTo<float>());
      if (pRotation && pRotation->CanConvertTo<float>())
        attr.m_UV.m_Rotation = plAngle::MakeFromDegree(pRotation->ConvertTo<float>());
      if (pHard && pHard->CanConvertTo<bool>())
      {
        attr.m_uiMask |= plBrushFaceAttributes::Flags;
        attr.m_uiFlags = pHard->ConvertTo<bool>() ? plBrushFaceFlags::HardEdges : 0;
      }

      if (!sJustify.IsEmpty())
      {
        // extents on the projection axes the face will use (local space: texture lock on)
        const plBrushFaceUV oldUV = face.m_UV;
        face.m_UV = attr.m_UV;
        plVec3 vU, vV;
        mesh.GetFaceUVAxes(uiFace, vNormal, vU, vV);
        face.m_UV = oldUV;

        float fMinU = plMath::MaxValue<float>(), fMaxU = -plMath::MaxValue<float>(), fMinV = plMath::MaxValue<float>(), fMaxV = -plMath::MaxValue<float>();
        for (plUInt32 c : face.m_Corners)
        {
          fMinU = plMath::Min(fMinU, mesh.m_Vertices[c].Dot(vU));
          fMaxU = plMath::Max(fMaxU, mesh.m_Vertices[c].Dot(vU));
          fMinV = plMath::Min(fMinV, mesh.m_Vertices[c].Dot(vV));
          fMaxV = plMath::Max(fMaxV, mesh.m_Vertices[c].Dot(vV));
        }

        auto frac = [](float f)
        { return f - plMath::Floor(f); };
        plVec2& s = attr.m_UV.m_vScale;
        plVec2& o = attr.m_UV.m_vOffset;

        if (sJustify == "left")
          o.x = frac(-fMinU * s.x);
        else if (sJustify == "right")
          o.x = frac(-fMaxU * s.x);
        else if (sJustify == "top")
          o.y = frac(-fMinV * s.y);
        else if (sJustify == "bottom")
          o.y = frac(-fMaxV * s.y);
        else if (sJustify == "center")
        {
          o.x = frac(0.5f - 0.5f * (fMinU + fMaxU) * s.x);
          o.y = frac(0.5f - 0.5f * (fMinV + fMaxV) * s.y);
        }
        else if (sJustify == "fit")
        {
          s.x = 1.0f / plMath::Max(fMaxU - fMinU, 0.0001f);
          s.y = 1.0f / plMath::Max(fMaxV - fMinV, 0.0001f);
          o.x = -fMinU * s.x;
          o.y = -fMinV * s.y;
        }
      }
    });

  Finish(out_result, status);
}

void plMcpModelingTool::Csg(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  if (pDocument == nullptr)
  {
    out_result.SetError("Needs an open scene 'document'.");
    return;
  }

  plDynamicArray<const plDocumentObject*> brushes;
  ResolveBrushes(pDocument, args, brushes);

  const plStringView sOp = plMcpJson::GetString(args, "operation");

  if (sOp == "toggle_cutter")
    Finish(out_result, plBrushCommands::ToggleCutter(pDocument, brushes));
  else if (sOp == "bake_cuts")
    Finish(out_result, plBrushCommands::BakeCuts(pDocument, brushes));
  else if (sOp == "hollow")
    Finish(out_result, plBrushCommands::Hollow(pDocument, brushes));
  else if (sOp == "union")
    Finish(out_result, plBrushCommands::Union(pDocument, brushes));
  else if (sOp == "decompose")
    Finish(out_result, plBrushCommands::Decompose(pDocument, brushes));
  else if (sOp == "group")
  {
    plUuid group;
    const plStatus status = plBrushCommands::CreateGroup(pDocument, brushes, &group);
    if (status.Failed())
    {
      Finish(out_result, status);
      return;
    }

    plMcpJsonWriter writer;
    writer.BeginObject();
    writer.AddVariableString("group", GuidString(group));
    writer.EndObject();
    out_result.m_sText = writer.GetResult();
  }
  else if (sOp == "clip")
  {
    const plVariantArray* pPlane = GetArray(args, "plane");
    if (pPlane == nullptr || pPlane->GetCount() != 4)
    {
      out_result.SetError("clip needs 'plane' as [nx, ny, nz, d].");
      return;
    }

    plPlane plane;
    plane.m_vNormal.Set((*pPlane)[0].ConvertTo<float>(), (*pPlane)[1].ConvertTo<float>(), (*pPlane)[2].ConvertTo<float>());
    plane.m_vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
    plane.m_fNegDistance = (*pPlane)[3].ConvertTo<float>();

    const plStringView sKeep = plMcpJson::GetString(args, "keep", "front");
    const auto keep = sKeep == "back" ? plBrushCommands::ClipKeep::Back : sKeep == "both" ? plBrushCommands::ClipKeep::Both : plBrushCommands::ClipKeep::Front;

    Finish(out_result, plBrushCommands::Clip(pDocument, brushes, plane, keep));
  }
  else
  {
    out_result.SetError("Unknown 'operation'. Use toggle_cutter, bake_cuts, hollow, union, decompose, group or clip.");
  }
}

void plMcpModelingTool::MeshOp(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  const plDocumentObject* pBrush = pDocument ? ResolveBrush(pDocument, plMcpJson::GetString(args, "brush")) : nullptr;

  if (pBrush == nullptr)
  {
    out_result.SetError("Needs an open scene 'document' and a 'brush' guid.");
    return;
  }

  plHashSet<plUInt32> faceIds, vertices;
  plHashSet<plUInt64> edges;
  GetUIntSet(args, "faceIds", faceIds);
  GetUIntSet(args, "vertices", vertices);
  GetEdges(args, "edges", edges);

  const float fAmount = static_cast<float>(GetNumber(args, "amount", 0.5));
  const plUInt32 uiSegments = static_cast<plUInt32>(plMcpJson::GetInt(args, "segments", 1));
  const float fPosition = static_cast<float>(GetNumber(args, "position", 0.5));
  const plStringView sOp = plMcpJson::GetString(args, "operation");

  plHybridArray<plUInt64, 16> edgeList;
  for (plUInt64 e : edges)
    edgeList.PushBack(e);

  auto firstEdge = [&](plUInt32& v0, plUInt32& v1)
  {
    if (edgeList.IsEmpty())
      return false;
    v0 = static_cast<plUInt32>(edgeList[0] & 0xFFFFFFFF);
    v1 = static_cast<plUInt32>(edgeList[0] >> 32);
    return true;
  };

  if (sOp == "detach")
  {
    plUuid newBrush;
    const plStatus status = plBrushCommands::Detach(pDocument, pBrush, faceIds, &newBrush);
    if (status.Succeeded())
    {
      plMcpJsonWriter writer;
      writer.BeginObject();
      writer.AddVariableString("newBrush", GuidString(newBrush));
      writer.EndObject();
      out_result.m_sText = writer.GetResult();
      return;
    }
    Finish(out_result, status);
    return;
  }

  plBrushCommands::MeshEditFunc func;

  if (sOp == "extrude")
    func = [&](plEditableMesh& m) { return plMeshOps::Extrude(m, faceIds, fAmount, !plMcpJson::GetBool(args, "individual", false)); };
  else if (sOp == "inset")
    func = [&](plEditableMesh& m) { return plMeshOps::Inset(m, faceIds, fAmount); };
  else if (sOp == "subdivide")
    func = [&](plEditableMesh& m) { return plMeshOps::Subdivide(m, faceIds); };
  else if (sOp == "flip")
    func = [&](plEditableMesh& m) { return plMeshOps::Flip(m, faceIds); };
  else if (sOp == "triangulate")
    func = [&](plEditableMesh& m) { return plMeshOps::Triangulate(m, faceIds); };
  else if (sOp == "flatten")
    func = [&](plEditableMesh& m) { return plMeshOps::Flatten(m, faceIds); };
  else if (sOp == "delete")
    func = [&](plEditableMesh& m) { return plMeshOps::DeleteFaces(m, faceIds); };
  else if (sOp == "bridge")
    func = [&](plEditableMesh& m)
    {
      plHybridArray<plUInt32, 2> ids;
      for (plUInt32 id : faceIds)
        ids.PushBack(id);
      return ids.GetCount() == 2 ? plMeshOps::Bridge(m, ids[0], ids[1]) : plStatus("bridge needs two faceIds");
    };
  else if (sOp == "bevel")
    func = [&](plEditableMesh& m) { return plMeshOps::Bevel(m, edgeList, fAmount, uiSegments); };
  else if (sOp == "loop_cut")
    func = [&](plEditableMesh& m)
    {
      plUInt32 v0, v1;
      return firstEdge(v0, v1) ? plMeshOps::LoopCut(m, v0, v1, uiSegments, fPosition) : plStatus("loop_cut needs one edge");
    };
  else if (sOp == "fill_hole")
    func = [&](plEditableMesh& m)
    {
      plUInt32 v0, v1;
      return firstEdge(v0, v1) ? plMeshOps::FillHole(m, v0, v1) : plStatus("fill_hole needs one edge");
    };
  else if (sOp == "weld")
    func = [&](plEditableMesh& m) { return plMeshOps::Weld(m, vertices); };
  else if (sOp == "connect")
    func = [&](plEditableMesh& m)
    {
      plHybridArray<plUInt32, 2> v;
      for (plUInt32 i : vertices)
        v.PushBack(i);
      return v.GetCount() == 2 ? plMeshOps::Connect(m, v[0], v[1]) : plStatus("connect needs two vertices");
    };
  else if (sOp == "delete_vertices")
    func = [&](plEditableMesh& m) { return plMeshOps::DeleteVertices(m, vertices); };
  else
  {
    out_result.SetError("Unknown 'operation'.");
    return;
  }

  plStringBuilder sAction;
  sAction.SetFormat("MCP {}", sOp);
  Finish(out_result, plBrushCommands::EditMesh(pDocument, pBrush, sAction, func));
}

void plMcpModelingTool::Tool(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  if (pDocument == nullptr)
  {
    out_result.SetError("Needs an open scene 'document'.");
    return;
  }

  // selecting the brush's object first, as a user would
  const plStringView sBrush = plMcpJson::GetString(args, "brush");
  if (!sBrush.IsEmpty())
  {
    if (const plDocumentObject* pBrush = ResolveBrush(pDocument, sBrush))
      pDocument->GetSelectionManager()->SetSelection(pBrush->GetParent());
  }

  plBrushToolContext* pContext = GetBrushContext(pDocument, true);
  if (pContext == nullptr)
  {
    out_result.SetError("The Brush tool is not available in this document.");
    return;
  }

  const plStringView sMode = plMcpJson::GetString(args, "mode");
  if (sMode == "object")
    pContext->SetSubMode(plBrushSubMode::Object);
  else if (sMode == "vertex")
    pContext->SetSubMode(plBrushSubMode::Vertex);
  else if (sMode == "edge")
    pContext->SetSubMode(plBrushSubMode::Edge);
  else if (sMode == "face")
    pContext->SetSubMode(plBrushSubMode::Face);

  if (args.Contains("faceIds") || args.Contains("vertices") || args.Contains("edges"))
  {
    plHashSet<plUInt32> faces, verts;
    plHashSet<plUInt64> edgeKeys;
    GetUIntSet(args, "faceIds", faces);
    GetUIntSet(args, "vertices", verts);

    if (const plVariantArray* pArray = GetArray(args, "edges"))
    {
      for (const plVariant& v : *pArray)
      {
        if (v.IsA<plVariantArray>() && v.Get<plVariantArray>().GetCount() == 2)
          edgeKeys.Insert(plBrushToolContext::MakeEdgeKey(v.Get<plVariantArray>()[0].ConvertTo<plUInt32>(), v.Get<plVariantArray>()[1].ConvertTo<plUInt32>()));
      }
    }

    pContext->SetSelection(verts, edgeKeys, faces);
  }

  const plStringView sSelOp = plMcpJson::GetString(args, "selectionOp");
  const struct
  {
    const char* m_szName;
    plBrushSelectionOp::Enum m_Op;
  } selOps[] = {{"all", plBrushSelectionOp::All}, {"none", plBrushSelectionOp::None}, {"invert", plBrushSelectionOp::Invert}, {"grow", plBrushSelectionOp::Grow},
    {"shrink", plBrushSelectionOp::Shrink}, {"loop", plBrushSelectionOp::Loop}, {"ring", plBrushSelectionOp::Ring}, {"same_material", plBrushSelectionOp::SameMaterial}};

  for (const auto& op : selOps)
  {
    if (sSelOp == op.m_szName)
      pContext->ApplySelectionOp(op.m_Op);
  }

  plStatus status(PL_SUCCESS);
  const plStringView sCommand = plMcpJson::GetString(args, "command");
  if (!sCommand.IsEmpty())
  {
    // modal commands follow the mouse in the view the user last hovered; a client never hovers, so it gets
    // the scene window's first perspective view
    if (plEngineDocumentWindow* pWindow = dynamic_cast<plEngineDocumentWindow*>(plDocumentWindow::FindWindowByDocument(pDocument)))
    {
      plEngineViewWidget::InteractionContext ctxt = plEngineViewWidget::GetInteractionContext();

      if (ctxt.m_pLastHoveredViewWidget == nullptr || !pWindow->GetViewWidgets().Contains(ctxt.m_pLastHoveredViewWidget))
      {
        for (plEngineViewWidget* pView : pWindow->GetViewWidgets())
        {
          if (pView->m_pViewConfig->m_Perspective == plSceneViewPerspective::Perspective)
          {
            ctxt.m_pLastHoveredViewWidget = pView;
            plEngineViewWidget::SetInteractionContext(ctxt);
            break;
          }
        }
      }
    }

    bool bFound = false;
    for (plUInt32 i = 0; i < plModelingCommand::Count; ++i)
    {
      if (sCommand.IsEqual_NoCase(plBrushActions::GetDisplayName(static_cast<plModelingCommand::Enum>(i))))
      {
        status = plBrushActions::Execute(pDocument, static_cast<plModelingCommand::Enum>(i));
        bFound = true;
        break;
      }
    }

    if (!bFound)
    {
      out_result.SetError("Unknown 'command'. Use a name as shown in the shortcut editor's Modeling category, e.g. 'Extrude'.");
      return;
    }
  }

  const char* szModes[] = {"object", "vertex", "edge", "face"};

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("mode", szModes[pContext->GetSubMode()]);
  writer.AddVariableBool("modalOperationRunning", pContext->IsModalOperationActive());
  writer.AddVariableString("info", pContext->GetInfoText());
  if (status.Failed())
    writer.AddVariableString("commandMessage", status.m_sMessage);

  if (const plDocumentObject* pTarget = pContext->GetTargetBrush())
    writer.AddVariableString("brush", GuidString(pTarget->GetGuid()));

  writer.BeginArray("faceIds");
  for (plUInt32 id : pContext->GetSelectedFaces())
    writer.WriteUInt32(id);
  writer.EndArray();

  writer.BeginArray("vertices");
  for (plUInt32 v : pContext->GetSelectedVertices())
    writer.WriteUInt32(v);
  writer.EndArray();

  writer.BeginArray("edges");
  for (plUInt64 e : pContext->GetSelectedEdges())
  {
    writer.BeginArray();
    writer.WriteUInt32(static_cast<plUInt32>(e >> 32));
    writer.WriteUInt32(static_cast<plUInt32>(e & 0xFFFFFFFF));
    writer.EndArray();
  }
  writer.EndArray();

  writer.EndObject();
  out_result.m_sText = writer.GetResult();
}

void plMcpModelingTool::Export(const plVariantDictionary& args, plMcpToolResult& out_result)
{
  plDocument* pDocument = FindOpenDocument(plMcpJson::GetString(args, "document"));
  if (pDocument == nullptr)
  {
    out_result.SetError("Needs an open scene 'document'.");
    return;
  }

  const plStringView sOp = plMcpJson::GetString(args, "operation");

  plDynamicArray<const plDocumentObject*> brushes;
  ResolveBrushes(pDocument, args, brushes);

  if (sOp == "obj")
  {
    plString sFile;
    const plStatus status = plBrushExport::ExportObj(pDocument, brushes, plMcpJson::GetString(args, "path"), &sFile);
    Finish(out_result, status);
  }
  else if (sOp == "bake")
  {
    Finish(out_result, plBrushExport::BakeToMeshAsset(pDocument, brushes, plMcpJson::GetBool(args, "replace", true)));
  }
  else if (sOp == "convert_greybox")
  {
    Finish(out_result, plBrushExport::ConvertGreyBoxes(pDocument));
  }
  else if (sOp == "mesh_to_brush")
  {
    plHybridArray<const plDocumentObject*, 16> objects;
    if (const plVariantArray* pArray = GetArray(args, "objects"))
    {
      for (const plVariant& v : *pArray)
      {
        plUuid guid;
        if (plConversionUtils::TryConvertStringToUuid(v.ConvertTo<plString>(), guid))
        {
          if (const plDocumentObject* pObject = pDocument->GetObjectManager()->GetObject(guid))
            objects.PushBack(pObject);
        }
      }
    }

    Finish(out_result, plBrushExport::ConvertMeshesToBrushes(pDocument, objects));
  }
  else
  {
    out_result.SetError("Unknown 'operation'. Use obj, bake, convert_greybox or mesh_to_brush.");
  }
}


PL_STATICLINK_FILE(EditorPluginModelingMcp, EditorPluginModelingMcp_ModelingMcpTools);
