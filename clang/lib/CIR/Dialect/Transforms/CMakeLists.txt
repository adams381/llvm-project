add_subdirectory(TargetLowering)

add_clang_library(MLIRCIRTransforms
  CallConvCascade.cpp
  CallConvLowering.cpp
  CIRCanonicalize.cpp
  CIRSimplify.cpp
  CIRTransformUtils.cpp
  CXXABILowering.cpp
  EHABILowering.cpp
  TargetLowering.cpp
  FlattenCFG.cpp
  HoistAllocas.cpp
  LoweringPrepare.cpp
  GotoSolver.cpp
  IdiomRecognizer.cpp
  VerifyCallConvInvariant.cpp

  DEPENDS
  MLIRCIRPassIncGen

  LINK_LIBS PUBLIC
  clangAST
  clangBasic

  MLIRAnalysis
  MLIRIR
  MLIRPass
  MLIRTransformUtils

  MLIRCIR
  MLIRCIRInterfaces
  MLIRCIRTargetLowering
  MLIRABI
  LLVMABI
  MLIROpenACCDialect
  MLIROpenMPDialect
)
