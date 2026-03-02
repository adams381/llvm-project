add_mlir_unittest(MLIRABITests
  ABITypeMapperTest.cpp
)

mlir_target_link_libraries(MLIRABITests
  PRIVATE
  MLIRABI
  MLIRDataLayoutInterfaces
  MLIRDLTIDialect
  MLIRIR
)
