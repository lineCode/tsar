set(TRANSFORM_SOURCES Passes.cpp ExprPropagation.cpp Inline.cpp RenameLocal.cpp
  DeadDeclsElimination.cpp FormatPass.cpp OpenMPAutoPar.cpp
  SharedMemoryAutoPar.cpp DVMHSMAutoPar.cpp)

if(MSVC_IDE)
  file(GLOB_RECURSE TRANSFORM_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/tsar/Transform/Clang/*.h)
  file(GLOB_RECURSE TRANSFORM_INTERNAL_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)
  source_group(bcl FILES ${BCL_CORE_HEADERS})
endif()

add_library(TSARTransformClang STATIC
  ${TRANSFORM_SOURCES} ${TRANSFORM_HEADERS} ${TRANSFORM_INTERNAL_HEADERS})

if(NOT PACKAGE_LLVM)
  add_dependencies(TSARTransformClang ${CLANG_LIBS} ${LLVM_LIBS})
endif()
add_dependencies(TSARTransformClang DirectivesGen DiagnosticKinds
  IntrinsicsGen AttributesGen)
target_link_libraries(TSARTransformClang TSARAnalysisClang TSARTool BCL::Core)

set_target_properties(TSARTransformClang PROPERTIES
  FOLDER "${TSAR_LIBRARY_FOLDER}"
  COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
