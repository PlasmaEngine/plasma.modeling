#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginModeling/Brush/BrushCommands.h>
#include <EditorPluginModeling/Brush/BrushExport.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <Foundation/IO/FileSystem/DeferredFileWriter.h>
#include <Foundation/IO/OSFile.h>
#include <ModelImporter2/Importer/Importer.h>
#include <ModelImporter2/ModelImporter.h>
#include <ModelingPlugin/Modeling/MeshOps.h>
#include <RendererCore/Declarations.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>
#include <ToolsFoundation/Project/ToolsProject.h>

namespace
{
  // Data/Base/Materials/Common/Pattern.plMaterialAsset — what an empty brush material slot renders with
  const char* s_szDefaultBrushMaterial = "{ 1c47ee4c-0379-4280-85f5-b8cda61941d2 }";

  /// Plasma space (X forward, Y right, Z up) to the OBJ convention (right-handed, Y up, -Z forward). This is
  /// the inverse of the mesh importer's default ("Blender Y up") transform, so an exported file round-trips.
  plVec3 ToObj(const plVec3& v)
  {
    return plVec3(-v.y, v.z, v.x);
  }

  plString MaterialGuidOf(const plDocumentObject* pBrush, plUInt32 uiSlot)
  {
    const auto& acc = pBrush->GetTypeAccessor();

    if (static_cast<plInt32>(uiSlot) < acc.GetCount("Materials"))
    {
      const plString sGuid = acc.GetValue("Materials", uiSlot).ConvertTo<plString>();
      if (!sGuid.IsEmpty())
        return sGuid;
    }

    return s_szDefaultBrushMaterial;
  }

  plString MaterialNameOf(plStringView sGuid)
  {
    plUuid guid;
    if (plConversionUtils::TryConvertStringToUuid(sGuid, guid))
    {
      if (auto pAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid))
      {
        plStringBuilder sName = pAsset->GetName();
        sName.ReplaceAll(" ", "_");
        return sName;
      }
    }

    return "Material";
  }

  plString SanitizeFileName(plStringView sName)
  {
    plStringBuilder s = sName;
    for (const char* szBad : {" ", "/", "\\", ":", "*", "?", "\"", "<", ">", "|"})
      s.ReplaceAll(szBad, "_");
    return s.IsEmpty() ? plString("Brush") : plString(s);
  }

  /// The brush as rendered, turned into triangles: smoothing and projected UVs exactly as the engine does.
  struct RenderedBrush
  {
    plGeometry m_Geometry;
    plDynamicArray<plBrushSubMeshRange> m_Ranges;
    const plDocumentObject* m_pBrush = nullptr;
  };

  plResult BuildRendered(const plDocumentObject* pBrush, RenderedBrush& out_rendered)
  {
    plEditableMesh mesh;
    if (plBrushPicking::BuildEvaluatedMeshFromObject(pBrush, mesh).Failed())
      return PL_FAILURE;

    const auto& acc = pBrush->GetTypeAccessor();
    const plAngle smoothing = acc.GetValue("SmoothingAngle").ConvertTo<plAngle>();
    const bool bTextureLock = acc.GetValue("TextureLock").ConvertTo<bool>();
    const plTransform global = plBrushPicking::GetComponentGlobalTransform(pBrush);

    out_rendered.m_pBrush = pBrush;
    mesh.BuildRenderGeometry(out_rendered.m_Geometry, out_rendered.m_Ranges, smoothing, bTextureLock ? nullptr : &global);
    return PL_SUCCESS;
  }

  /// Writes OBJ + MTL. Each brush's vertices go through its transform (identity keeps its own space).
  plStatus WriteObj(plStringView sObjPath, plArrayPtr<RenderedBrush> brushes, bool bWorldSpace, plDynamicArray<plString>* out_pMaterialOrder)
  {
    plStringBuilder sMtlPath = sObjPath;
    sMtlPath.ChangeFileExtension("mtl");

    plStringBuilder sMtlFileName = sMtlPath.GetFileNameAndExtension();

    // every distinct material, in first-use order; face groups follow this order
    plDynamicArray<plString> materials;

    plStringBuilder obj;
    obj.Append("# Written by the Plasma modeling package. Right-handed, Y up, metres.\n");
    obj.AppendFormat("mtllib {}\n", sMtlFileName);

    plUInt32 uiVertexBase = 1;

    for (const RenderedBrush& rendered : brushes)
    {
      const plTransform transform = bWorldSpace ? plBrushPicking::GetComponentGlobalTransform(rendered.m_pBrush) : plTransform::MakeIdentity();
      const auto& verts = rendered.m_Geometry.GetVertices();

      plStringBuilder sName = rendered.m_pBrush->GetParent()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
      obj.AppendFormat("o {}\n", SanitizeFileName(sName.IsEmpty() ? plStringView("Brush") : sName.GetView()));

      for (const auto& v : verts)
      {
        const plVec3 p = ToObj(transform.TransformPosition(v.m_vPosition));
        obj.AppendFormat("v {} {} {}\n", plArgF(p.x, 6), plArgF(p.y, 6), plArgF(p.z, 6));
      }

      for (const auto& v : verts)
      {
        // OBJ texture space has V pointing up
        obj.AppendFormat("vt {} {}\n", plArgF(v.m_vTexCoord.x, 6), plArgF(1.0f - v.m_vTexCoord.y, 6));
      }

      for (const auto& v : verts)
      {
        const plVec3 n = ToObj(transform.TransformDirection(v.m_vNormal).GetNormalized());
        obj.AppendFormat("vn {} {} {}\n", plArgF(n.x, 5), plArgF(n.y, 5), plArgF(n.z, 5));
      }

      for (const plBrushSubMeshRange& range : rendered.m_Ranges)
      {
        const plString sMaterial = MaterialGuidOf(rendered.m_pBrush, range.m_uiMaterialSlot);

        plUInt32 uiMaterial = materials.IndexOf(sMaterial);
        if (uiMaterial == plInvalidIndex)
        {
          uiMaterial = materials.GetCount();
          materials.PushBack(sMaterial);
        }

        obj.AppendFormat("usemtl m{}_{}\n", uiMaterial, MaterialNameOf(sMaterial));

        for (plUInt32 t = range.m_uiFirstTriangle; t < range.m_uiFirstTriangle + range.m_uiNumTriangles; ++t)
        {
          const auto& poly = rendered.m_Geometry.GetPolygons()[t];

          // the axis change mirrors, so the winding reverses to stay front-facing
          const plUInt32 a = uiVertexBase + poly.m_Vertices[2];
          const plUInt32 b = uiVertexBase + poly.m_Vertices[1];
          const plUInt32 c = uiVertexBase + poly.m_Vertices[0];
          obj.AppendFormat("f {0}/{0}/{0} {1}/{1}/{1} {2}/{2}/{2}\n", a, b, c);
        }
      }

      uiVertexBase += verts.GetCount();
    }

    plStringBuilder mtl;
    mtl.Append("# Written by the Plasma modeling package. Each material names its Plasma material asset.\n");

    for (plUInt32 i = 0; i < materials.GetCount(); ++i)
    {
      mtl.AppendFormat("newmtl m{}_{}\n", i, MaterialNameOf(materials[i]));
      mtl.AppendFormat("# plasma-material {}\n", materials[i]);
      mtl.Append("Kd 0.8 0.8 0.8\nKs 0 0 0\nd 1\nillum 1\n\n");
    }

    for (const auto& file : {std::make_pair(plStringView(sObjPath), &obj), std::make_pair(sMtlPath.GetView(), &mtl)})
    {
      plDeferredFileWriter writer;
      writer.SetOutput(file.first);
      writer.WriteBytes(file.second->GetData(), file.second->GetElementCount()).IgnoreResult();

      if (writer.Close().Failed())
        return plStatus(plFmt("Could not write '{}'.", file.first));
    }

    if (out_pMaterialOrder)
      *out_pMaterialOrder = materials;

    return plStatus(PL_SUCCESS);
  }

  plString GetSceneName(const plDocument* pDocument)
  {
    plStringBuilder s = plStringView(pDocument->GetDocumentPath()).GetFileName();
    return s;
  }

  void SetProperty(plCommandHistory* pHistory, const plUuid& object, plStringView sProperty, const plVariant& value)
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = object;
    cmd.m_sProperty = sProperty;
    cmd.m_NewValue = value;
    pHistory->AddCommand(cmd).LogFailure();
  }

  plUuid AddComponent(plCommandHistory* pHistory, const plUuid& owner, plStringView sType)
  {
    const plUuid guid = plUuid::MakeUuid();

    plAddObjectCommand cmd;
    cmd.m_NewObjectGuid = guid;
    cmd.m_pType = plRTTI::FindTypeByName(sType);
    cmd.m_sParentProperty = "Components";
    cmd.m_Parent = owner;
    cmd.m_Index = -1;

    if (cmd.m_pType == nullptr || pHistory->AddCommand(cmd).Failed())
      return plUuid();

    return guid;
  }

  void RemoveComponent(plCommandHistory* pHistory, const plUuid& component)
  {
    plRemoveObjectCommand cmd;
    cmd.m_Object = component;
    pHistory->AddCommand(cmd).LogFailure();
  }

  /// A success that carries a message for the status bar.
  plStatus Done(const plFormatString& message)
  {
    plStringBuilder sText;
    return plStatus(PL_SUCCESS, message.GetText(sText));
  }

  plDocument* FindOpenDocument(plStringView sPath)
  {
    for (plDocumentManager* pManager : plDocumentManager::GetAllDocumentManagers())
    {
      if (plDocument* pDoc = pManager->GetDocumentByPath(sPath))
        return pDoc;
    }

    return nullptr;
  }
} // namespace

plStatus plBrushExport::ExportObj(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, plStringView sPath, plString* out_pWrittenFile)
{
  if (brushes.IsEmpty())
    return plStatus("Export: select the brushes to export.");

  plDynamicArray<RenderedBrush> rendered;
  rendered.SetCount(brushes.GetCount());

  for (plUInt32 i = 0; i < brushes.GetCount(); ++i)
  {
    if (BuildRendered(brushes[i], rendered[i]).Failed())
      return plStatus("Export: a brush's stored mesh is damaged.");
  }

  plStringBuilder sFile = sPath;
  if (sFile.IsEmpty())
  {
    sFile = pDocument->GetDocumentPath();
    sFile.ChangeFileNameAndExtension("");
    sFile.AppendPath(GetSceneName(pDocument));
    sFile.Append("_Brushes.obj");
  }

  plStatus status = WriteObj(sFile, rendered, true, nullptr);

  if (status.Succeeded())
  {
    if (out_pWrittenFile)
      *out_pWrittenFile = sFile;

    plLog::Success("Exported {} brush(es) to '{}'.", brushes.GetCount(), sFile);
    return Done(plFmt("Exported to {}", sFile));
  }

  return status;
}

plStatus plBrushExport::BakeToMeshAsset(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, bool bReplace)
{
  if (brushes.IsEmpty())
    return plStatus("Bake: select the brushes to bake.");

  struct Baked
  {
    const plDocumentObject* m_pBrush = nullptr;
    plUuid m_AssetGuid;
  };

  plDynamicArray<Baked> baked;

  plStringBuilder sDir = plToolsProject::GetSingleton()->GetProjectDirectory();
  sDir.AppendPath("Blockout");

  if (plOSFile::CreateDirectoryStructure(sDir).Failed())
    return plStatus(plFmt("Bake: could not create '{}'.", sDir));

  for (const plDocumentObject* pBrush : brushes)
  {
    RenderedBrush rendered;
    if (BuildRendered(pBrush, rendered).Failed())
      return plStatus("Bake: a brush's stored mesh is damaged.");

    if (rendered.m_Ranges.IsEmpty())
      continue;

    // a unique file per brush
    const plString sObjectName = pBrush->GetParent()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    plStringBuilder sBase = GetSceneName(pDocument);
    sBase.Append("_", SanitizeFileName(sObjectName));

    plStringBuilder sObjPath, sAssetPath;
    for (plUInt32 uiSuffix = 0;; ++uiSuffix)
    {
      sObjPath = sDir;
      sObjPath.AppendPath(sBase);
      if (uiSuffix > 0)
        sObjPath.AppendFormat("_{}", uiSuffix);

      sAssetPath = sObjPath;
      sObjPath.Append(".obj");
      sAssetPath.Append(".plMeshAsset");

      if (!plOSFile::ExistsFile(sObjPath) && !plOSFile::ExistsFile(sAssetPath))
        break;
    }

    plDynamicArray<plString> materialOrder;
    plStatus status = WriteObj(sObjPath, plArrayPtr<RenderedBrush>(&rendered, 1), false, &materialOrder);
    if (status.Failed())
      return status;

    // the mesh asset: the OBJ with the default import transform, the brush's materials in its slots
    plDocument* pAssetDoc = plEditorApp::GetSingleton()->CreateDocument(sAssetPath, plDocumentFlags::None);
    if (pAssetDoc == nullptr)
      return plStatus(plFmt("Bake: could not create '{}'.", sAssetPath));

    plStringBuilder sRelativeObj = sObjPath;
    plEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sRelativeObj);

    {
      const plDocumentObject* pPropObj = pAssetDoc->GetObjectManager()->GetRootObject()->GetChildren()[0];
      plObjectCommandAccessor ca(pAssetDoc->GetCommandHistory());
      ca.StartTransaction("Init Values");
      ca.SetValueByName(pPropObj, "MeshFile", plString(sRelativeObj)).LogFailure();
      ca.SetValueByName(pPropObj, "ImportMaterials", false).LogFailure();

      for (plUInt32 i = 0; i < materialOrder.GetCount(); ++i)
      {
        plUuid slotGuid = plUuid::MakeUuid();
        ca.AddObjectByName(pPropObj, "Materials", i, plRTTI::FindTypeByName("plMaterialResourceSlot"), slotGuid).LogFailure();

        const plDocumentObject* pSlot = ca.GetObject(slotGuid);
        plStringBuilder sLabel;
        sLabel.SetFormat("m{}_{}", i, MaterialNameOf(materialOrder[i]));
        ca.SetValueByName(pSlot, "Label", plString(sLabel)).LogFailure();
        ca.SetValueByName(pSlot, "Resource", materialOrder[i]).LogFailure();
      }

      ca.FinishTransaction();
    }

    const plUuid assetGuid = pAssetDoc->GetGuid();
    pAssetDoc->SaveDocument(true).LogFailure();
    pAssetDoc->GetDocumentManager()->CloseDocument(pAssetDoc);

    plAssetCurator::GetSingleton()->TransformAsset(assetGuid, plTransformFlags::None);

    Baked& b = baked.ExpandAndGetRef();
    b.m_pBrush = pBrush;
    b.m_AssetGuid = assetGuid;

    plLog::Success("Baked '{}' to '{}'.", sObjectName, sAssetPath);
  }

  if (bReplace && !baked.IsEmpty())
  {
    plCommandHistory* pHistory = pDocument->GetCommandHistory();
    pHistory->StartTransaction("Replace Brushes With Meshes");

    for (const Baked& b : baked)
    {
      const plUuid meshComp = AddComponent(pHistory, b.m_pBrush->GetParent()->GetGuid(), "plMeshComponent");

      if (meshComp.IsValid())
      {
        plStringBuilder sGuid;
        plConversionUtils::ToString(b.m_AssetGuid, sGuid);
        SetProperty(pHistory, meshComp, "Mesh", plString(sGuid));
        RemoveComponent(pHistory, b.m_pBrush->GetGuid());
      }
    }

    pHistory->FinishTransaction();
  }

  return Done(plFmt("Baked {} brush(es) into mesh assets under Blockout/.", baked.GetCount()));
}

plStatus plBrushExport::ConvertGreyBoxes(plDocument* pDocument)
{
  // every Grey Boxing component in the scene
  plDynamicArray<const plDocumentObject*> greyBoxes;
  {
    plDynamicArray<const plDocumentObject*> stack;
    stack.PushBack(pDocument->GetObjectManager()->GetRootObject());

    while (!stack.IsEmpty())
    {
      const plDocumentObject* pCurrent = stack.PeekBack();
      stack.PopBack();

      for (const plDocumentObject* pChild : pCurrent->GetChildren())
      {
        if (pChild->GetTypeAccessor().GetType()->GetTypeName() == "plGreyBoxComponent")
          greyBoxes.PushBack(pChild);

        stack.PushBack(pChild);
      }
    }
  }

  if (greyBoxes.IsEmpty())
    return plStatus(PL_SUCCESS, "No Grey Boxing objects in this scene.");

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("Convert Grey Boxing To Brushes");

  plUInt32 uiSlopedSkipped = 0;

  for (const plDocumentObject* pGrey : greyBoxes)
  {
    const auto& acc = pGrey->GetTypeAccessor();
    const plUuid brush = AddComponent(pHistory, pGrey->GetParent()->GetGuid(), "plBrushComponent");

    if (!brush.IsValid())
      continue;

    // the shape enums share their order
    SetProperty(pHistory, brush, "Shape", acc.GetValue("Shape").ConvertTo<plInt64>());

    for (const char* szProp : {"SizeNegX", "SizePosX", "SizeNegY", "SizePosY", "SizeNegZ", "SizePosZ", "Detail", "Curvature", "Thickness", "Color", "CustomData",
           "GenerateCollision", "IncludeInNavmesh", "UseAsOccluder"})
    {
      SetProperty(pHistory, brush, szProp, acc.GetValue(szProp));
    }

    const plString sMaterial = acc.GetValue("Material").ConvertTo<plString>();
    if (!sMaterial.IsEmpty())
    {
      plInsertObjectPropertyCommand cmd;
      cmd.m_Object = brush;
      cmd.m_sProperty = "Materials";
      cmd.m_Index = 0;
      cmd.m_NewValue = sMaterial;
      pHistory->AddCommand(cmd).LogFailure();
    }

    if (acc.GetValue("SlopedTop").ConvertTo<bool>() || acc.GetValue("SlopedBottom").ConvertTo<bool>())
      ++uiSlopedSkipped;

    RemoveComponent(pHistory, pGrey->GetGuid());
  }

  pHistory->FinishTransaction();

  if (uiSlopedSkipped > 0)
    return Done(plFmt("Converted {} Grey Boxing objects. {} sloped spiral(s) became stepped spirals.", greyBoxes.GetCount(), uiSlopedSkipped));

  return Done(plFmt("Converted {} Grey Boxing objects to brushes.", greyBoxes.GetCount()));
}

plStatus plBrushExport::ConvertMeshesToBrushes(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> objects)
{
  struct Conversion
  {
    const plDocumentObject* m_pMeshComponent = nullptr;
    plEditableMesh m_Mesh;
    plDynamicArray<plString> m_Materials;
  };

  plDynamicArray<Conversion> conversions;

  for (const plDocumentObject* pObject : objects)
  {
    const plDocumentObject* pMeshComp = nullptr;

    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetTypeAccessor().GetType()->GetTypeName() == "plMeshComponent")
      {
        pMeshComp = pChild;
        break;
      }
    }

    if (pMeshComp == nullptr)
      continue;

    const plString sMeshGuid = pMeshComp->GetTypeAccessor().GetValue("Mesh").ConvertTo<plString>();

    plUuid meshGuid;
    if (!plConversionUtils::TryConvertStringToUuid(sMeshGuid, meshGuid))
      continue;

    plStringBuilder sAssetPath;
    {
      auto pAsset = plAssetCurator::GetSingleton()->GetSubAsset(meshGuid);
      if (!pAsset)
        continue;
      sAssetPath = pAsset->m_pAssetInfo->m_Path.GetAbsolutePath();
    }

    // the mesh asset's source file and import settings
    plDocument* pAssetDoc = FindOpenDocument(sAssetPath);
    const bool bWasOpen = pAssetDoc != nullptr;

    if (!bWasOpen)
      pAssetDoc = plEditorApp::GetSingleton()->OpenDocument(sAssetPath, plDocumentFlags::None);

    if (pAssetDoc == nullptr)
      continue;

    const plDocumentObject* pProps = pAssetDoc->GetObjectManager()->GetRootObject()->GetChildren()[0];
    const auto& props = pProps->GetTypeAccessor();

    plStringBuilder sMeshFile = props.GetValue("MeshFile").ConvertTo<plString>();
    const auto importTransform = static_cast<plMeshImportTransform::Enum>(props.GetValue("ImportTransform").ConvertTo<plInt32>());
    const auto rightDir = plMeshImportTransform::GetRightDir(importTransform, static_cast<plBasisAxis::Enum>(props.GetValue("RightDir").ConvertTo<plInt32>()));
    const auto upDir = plMeshImportTransform::GetUpDir(importTransform, static_cast<plBasisAxis::Enum>(props.GetValue("UpDir").ConvertTo<plInt32>()));
    const bool bFlip = plMeshImportTransform::GetFlipForward(importTransform, props.GetValue("FlipForwardDir").ConvertTo<bool>());
    const float fScale = plMath::Clamp(props.GetValue("UniformScaling").ConvertTo<float>(), 0.0001f, 10000.0f);

    Conversion& conv = conversions.ExpandAndGetRef();
    conv.m_pMeshComponent = pMeshComp;

    // asset slots, overridden per slot by the component
    for (plInt32 i = 0; i < props.GetCount("Materials"); ++i)
    {
      const plUuid slotGuid = props.GetValue("Materials", i).ConvertTo<plUuid>();
      const plDocumentObject* pSlot = pAssetDoc->GetObjectManager()->GetObject(slotGuid);
      conv.m_Materials.PushBack(pSlot ? pSlot->GetTypeAccessor().GetValue("Resource").ConvertTo<plString>() : plString());
    }

    const auto& compAcc = pMeshComp->GetTypeAccessor();
    for (plInt32 i = 0; i < compAcc.GetCount("Materials"); ++i)
    {
      const plString sOverride = compAcc.GetValue("Materials", i).ConvertTo<plString>();
      if (!sOverride.IsEmpty())
      {
        conv.m_Materials.EnsureCount(i + 1);
        conv.m_Materials[i] = sOverride;
      }
    }

    if (!bWasOpen)
      pAssetDoc->GetDocumentManager()->CloseDocument(pAssetDoc);

    if (!plEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sMeshFile))
    {
      conversions.PopBack();
      continue;
    }

    plUniquePtr<plModelImporter2::Importer> pImporter = plModelImporter2::RequestImporterForFileType(sMeshFile);
    if (pImporter == nullptr)
    {
      conversions.PopBack();
      continue;
    }

    plMeshResourceDescriptor desc;
    plModelImporter2::ImportOptions opt;
    opt.m_sSourceFile = sMeshFile;
    opt.m_RootTransform = plBasisAxis::CalculateTransformationMatrix(plBasisAxis::GetOrthogonalAxis(rightDir, upDir, !bFlip), rightDir, upDir, fScale);
    opt.m_pMeshOutput = &desc;

    if (pImporter->Import(opt).Failed())
    {
      conversions.PopBack();
      continue;
    }

    // triangles become faces with their imported UVs kept per corner
    const auto& buffer = desc.MeshBufferDesc();
    const bool bHasUV = !buffer.GetTexCoord0Data().IsEmpty();
    const plArrayPtr<const plUInt8> indices = buffer.GetIndexBufferData();
    const bool b32 = buffer.Uses32BitIndices();

    auto index = [&](plUInt32 i) -> plUInt32
    { return b32 ? reinterpret_cast<const plUInt32*>(indices.GetPtr())[i] : reinterpret_cast<const plUInt16*>(indices.GetPtr())[i]; };

    plEditableMesh& mesh = conv.m_Mesh;
    mesh.BeginShell(false);

    for (plUInt32 v = 0; v < buffer.GetVertexCount(); ++v)
      mesh.AddVertex(buffer.GetPosition(v));

    for (const auto& sub : desc.GetSubMeshes())
    {
      for (plUInt32 t = sub.m_uiFirstPrimitive; t < sub.m_uiFirstPrimitive + sub.m_uiPrimitiveCount; ++t)
      {
        const plUInt32 corners[3] = {index(t * 3 + 0), index(t * 3 + 1), index(t * 3 + 2)};
        const plUInt32 uiFace = mesh.AddFace(plArrayPtr<const plUInt32>(corners), mesh.m_uiNextFaceId, static_cast<plUInt8>(plMath::Min<plUInt32>(sub.m_uiMaterialIndex, 255)));

        if (bHasUV)
        {
          plBrushFace& face = mesh.m_Faces[uiFace];
          face.m_uiFlags |= plBrushFaceFlags::ExplicitUV;
          face.m_CornerUVs.SetCount(3);
          for (plUInt32 k = 0; k < 3; ++k)
            face.m_CornerUVs[k] = buffer.GetTexCoord0(corners[k]);
        }
      }
    }

    // split UV seams share positions again, so the brush edits as one surface
    plMeshOps::WeldByDistance(mesh, 0.0001f);
  }

  if (conversions.IsEmpty())
    return plStatus("Mesh to brush: select objects with mesh components whose source file can be read.");

  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  pHistory->StartTransaction("Convert Meshes To Brushes");

  for (const Conversion& conv : conversions)
  {
    const plUuid brush = AddComponent(pHistory, conv.m_pMeshComponent->GetParent()->GetGuid(), "plBrushComponent");
    if (!brush.IsValid())
      continue;

    for (plUInt32 i = 0; i < conv.m_Materials.GetCount(); ++i)
    {
      plInsertObjectPropertyCommand cmd;
      cmd.m_Object = brush;
      cmd.m_sProperty = "Materials";
      cmd.m_Index = static_cast<plInt32>(i);
      cmd.m_NewValue = conv.m_Materials[i];
      pHistory->AddCommand(cmd).LogFailure();
    }

    plDataBuffer blob;
    conv.m_Mesh.ToDataBuffer(blob);
    SetProperty(pHistory, brush, "Shape", static_cast<plInt64>(plBrushShape::Editable));
    SetProperty(pHistory, brush, "StoredMesh", blob);

    // imported meshes carry their own normals; hard 0 would facet curved models
    SetProperty(pHistory, brush, "SmoothingAngle", plAngle::MakeFromDegree(60.0f));

    RemoveComponent(pHistory, conv.m_pMeshComponent->GetGuid());
  }

  pHistory->FinishTransaction();

  return Done(plFmt("Converted {} mesh(es) into brushes.", conversions.GetCount()));
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushExport);
