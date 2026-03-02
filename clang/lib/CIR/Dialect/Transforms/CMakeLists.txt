add_subdirectory(TargetLowering)

add_clang_library(MLIRCIRTransforms
  CallConvLowering.cpp
  CIRCanonicalize.cpp
  CIRSimplify.cpp
  CXXABILowering.cpp
  TargetLowering.cpp
  FlattenCFG.cpp
  HoistAllocas.cpp
  LoweringPrepare.cpp
  GotoSolver.cpp

  DEPENDS
  MLIRCIRPassIncGen

  LINK_LIBS PUBLIC
  clangAST
  clangBasic

  MLIRABI
  MLIRAnalysis
  MLIRDataLayoutInterfaces
  MLIRFunctionInterfaces
  MLIRIR
  MLIRPass
  MLIRTransformUtils

  MLIRCIR
  MLIRCIRInterfaces
  MLIRCIRTargetLowering
)
