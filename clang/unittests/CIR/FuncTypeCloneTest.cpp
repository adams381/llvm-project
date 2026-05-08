//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Unit tests for FuncType::clone, which must accept both 0 and 1 result
// types (the FunctionOpInterface views a void return as an empty type
// range while CIR's FuncType stores a single VoidType).
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/MLIRContext.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"
#include "gtest/gtest.h"

using namespace mlir;
using namespace cir;

class FuncTypeCloneTest : public ::testing::Test {
protected:
  FuncTypeCloneTest() { context.loadDialect<cir::CIRDialect>(); }

  MLIRContext context;
};

TEST_F(FuncTypeCloneTest, CloneWithVoidResultRange) {
  // A void function: stored internally as a single VoidType return.
  IntType i32 = IntType::get(&context, 32, true);
  VoidType voidTy = VoidType::get(&context);
  FuncType fn = FuncType::get(SmallVector<Type>{i32}, voidTy);
  ASSERT_EQ(fn.getReturnTypes().size(), 0u);

  // FunctionOpInterface::cloneTypeWith calls back here with an empty
  // result range for void functions.  Must not assert.
  FuncType cloned = fn.clone({i32}, {});
  EXPECT_TRUE(isa<VoidType>(cloned.getReturnType()));
  EXPECT_EQ(cloned.getInputs().size(), 1u);
}

TEST_F(FuncTypeCloneTest, CloneWithSingleResult) {
  IntType i32 = IntType::get(&context, 32, true);
  IntType i64 = IntType::get(&context, 64, true);
  FuncType fn = FuncType::get(SmallVector<Type>{i32}, i64);

  FuncType cloned = fn.clone({i32}, {i32});
  EXPECT_EQ(cloned.getReturnType(), i32);
}

TEST_F(FuncTypeCloneTest, CloneVarArgPreserved) {
  IntType i32 = IntType::get(&context, 32, true);
  VoidType voidTy = VoidType::get(&context);
  FuncType fn = FuncType::get(SmallVector<Type>{i32}, voidTy,
                              /*isVarArg=*/true);

  FuncType cloned = fn.clone({i32}, {});
  EXPECT_TRUE(cloned.isVarArg());
  EXPECT_TRUE(isa<VoidType>(cloned.getReturnType()));
}
