//===- APCContext.h - Class for managing parallelization state  -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file declares APCContext, a container of a state of automated
// parallelization process in SAPFOR.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_APC_CONTEXT_H
#define TSAR_APC_CONTEXT_H

#include "AnalysisWrapperPass.h"
#include "tsar_utility.h"
#include <apc/apc-config.h>
#include <bcl/utility.h>

struct LoopGraph;
struct ParallelRegion;
class Statement;

namespace apc {
using LoopGraph = ::LoopGraph;
using ParallelRegion = ::ParallelRegion;
using Statement = ::Statement;
}

namespace tsar {
struct APCContextImpl;

class APCContext final : private bcl::Uncopyable {
public:
  APCContext();
  ~APCContext();

  /// Initialize context.
  ///
  /// We perform separate initialization to prevent memory leak. Constructor
  /// performs allocation of context storage only.
  void initialize();

  /// Returns default region which is a whole program.
  ParallelRegion & getDefaultRegion();

  /// Add loop with a specified ID under of APCContext control.
  ///
  /// If ManageMemory is set to true then context releases memory when it
  /// becomes unused.
  /// \return `false` if loop with a specified ID already exists.
  bool addLoop(LoopID ID, apc::LoopGraph *L, bool ManageMemory = false);

  APCContextImpl * const mImpl;
#ifndef NDEBUG
  bool mIsInitialized = false;
#endif
};
}

namespace llvm {
/// Wrapper to access auto-parallelization context.
using APCContextWrapper = AnalysisWrapperPass<tsar::APCContext>;
}

#endif//TSAR_APC_CONTEXT_H