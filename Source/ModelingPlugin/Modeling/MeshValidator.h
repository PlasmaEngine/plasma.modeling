#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief What is wrong with a mesh, in terms a level designer can act on.
struct PL_MODELINGPLUGIN_DLL plMeshValidationReport
{
  plUInt32 m_uiNonPlanarFaces = 0;    ///< corners off the face plane by more than the tolerance
  plUInt32 m_uiConcaveFaces = 0;      ///< valid, but they make the brush unusable as a cutter
  plUInt32 m_uiDegenerateFaces = 0;   ///< (near) zero area
  plUInt32 m_uiOpenEdges = 0;         ///< edges with a single face: the surface has holes
  plUInt32 m_uiNonManifoldEdges = 0;  ///< edges shared by more than two faces, or inconsistently wound
  plUInt32 m_uiNonConvexShells = 0;
  bool m_bStructurallyValid = true;

  plDynamicArray<plUInt32> m_NonPlanarFaceIds;

  bool HasProblems() const { return !m_bStructurallyValid || m_uiNonPlanarFaces > 0 || m_uiDegenerateFaces > 0 || m_uiOpenEdges > 0 || m_uiNonManifoldEdges > 0; }
};

namespace plMeshValidator
{
  /// \brief Fills the report. Shell convexity (m_uiNonConvexShells) is only computed with
  /// bCheckShellConvexity: it only matters for cutters and costs faces x corners per shell.
  PL_MODELINGPLUGIN_DLL void Validate(const plEditableMesh& mesh, plMeshValidationReport& out_report, bool bCheckShellConvexity, float fPlanarityTolerance = 0.001f);
} // namespace plMeshValidator
