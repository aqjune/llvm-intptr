set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Analysis
  BitReader
  BitWriter
  CodeGen
  Core
  IRReader
  Instrumentation
  MC
  Support
  Target
  TransformUtils
  Passes
  )

add_llvm_tool(instcounter
  instcounter.cpp

  DEPENDS
  intrinsics_gen
  )
