// Author: Junyang

#define DEBUG_TYPE "dyn-aa"

#include <cstdio>
#include <fstream>

#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"

#include "rcs/IDAssigner.h"

#include "dyn-aa/LogCounter.h"
#include "dyn-aa/TraceSlicer.h"

using namespace std;
using namespace llvm;
using namespace rcs;
using namespace dyn_aa;

static cl::opt<unsigned> FirstPointerRecordID(
    "pt1",
    cl::desc("RecordID of the first pointer"));

static cl::opt<unsigned> SecondPointerRecordID(
    "pt2",
    cl::desc("RecordID of the second pointer"));

static RegisterPass<TraceSlicer> X("slice-trace",
                                   "Slice trace of two input pointers",
                                   false, // Is CFG Only?
                                   true); // Is Analysis?

char TraceSlicer::ID = 0;

bool TraceSlicer::runOnModule(Module &M) {
  LogCounter LC;
  LC.processLog();
  CurrentRecordID = LC.getNumLogRecords();

  CurrentState[0].StartRecordID = FirstPointerRecordID;
  CurrentState[1].StartRecordID = SecondPointerRecordID;

  processLog(true);

  return false;
}

void TraceSlicer::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<IDAssigner>();
}

void TraceSlicer::printTrace(raw_ostream &O,
                             pair<unsigned, unsigned> TraceRecord,
                             int PointerLabel) const {
  unsigned RecordID = TraceRecord.first;
  unsigned ValueID = TraceRecord.second;
  IDAssigner &IDA = getAnalysis<IDAssigner>();
  Value *V = IDA.getValue(ValueID);
  O << RecordID << "\t";
  O << "ptr" << PointerLabel + 1 << "\t";
  O << ValueID << "\t";
  DynAAUtils::PrintValue(O, V);
//  if (Argument *A = dyn_cast<Argument>(V)) {
//    O << " (argNo " << A->getArgNo() << ")";
//  }
  O << "\n";
}

void TraceSlicer::print(raw_ostream &O, const Module *M) const {
  O << "RecID\tPtr\tValueID\tFunc:  Inst/Arg\n\n";
  for (int PointerLabel = 0; PointerLabel < 2; ++PointerLabel) {
    O << "ptr" << PointerLabel + 1 << ": \n";
    for (int i = CurrentState[PointerLabel].Trace.size() - 1; i >= 0; --i) {
      printTrace(O, CurrentState[PointerLabel].Trace[i], PointerLabel);
    }
    O << "\n";
  }

  O << "Merged: \n";
  int Index[2];
  Index[0] = CurrentState[0].Trace.size() - 1;
  Index[1] = CurrentState[1].Trace.size() - 1;
  while (true) {
    unsigned Min;
    int PointerLabel = -1;
    for (int i = 0; i < 2; ++i) {
      if (Index[i] >= 0 &&
          (PointerLabel == -1 || CurrentState[i].Trace[Index[i]].first < Min)) {
        Min = CurrentState[i].Trace[Index[i]].first;
        PointerLabel = i;
      }
    }
    if (PointerLabel != -1) {
      printTrace(O,
                 CurrentState[PointerLabel].Trace[Index[PointerLabel]],
                 PointerLabel);
      Index[PointerLabel]--;
    } else
      break;
  }
}

bool TraceSlicer::isLive(int PointerLabel) {
  if (CurrentState[PointerLabel].StartRecordID < CurrentRecordID ||
      CurrentState[PointerLabel].End) {
    return false;
  }
  return true;
}

void TraceSlicer::processAddrTakenDecl(const AddrTakenDeclLogRecord &Record) {
  CurrentRecordID--;
  for (int PointerLabel = 0; PointerLabel < 2; ++PointerLabel) {
    // Starting record must be a TopLevelPointTo record
    assert(CurrentState[PointerLabel].StartRecordID != CurrentRecordID);
  }
}

void TraceSlicer::processTopLevelPointTo(
    const TopLevelPointToLogRecord &Record) {
  CurrentRecordID--;
  int NumContainingSlices = 0;
  IDAssigner &IDA = getAnalysis<IDAssigner>();
  for (int PointerLabel = 0; PointerLabel < 2; ++PointerLabel) {
    if (CurrentState[PointerLabel].StartRecordID == CurrentRecordID) {
      Value *V = IDA.getValue(Record.PointerValueID);
      assert(V->getType()->isPointerTy());
      CurrentState[PointerLabel].ValueID = Record.PointerValueID;
    }
    if (isLive(PointerLabel)) {
      if (CurrentState[PointerLabel].Action != TopLevelPointTo) {
        continue;
      }

      if (!CurrentState[PointerLabel].ValueIDCandidates.empty()) {
        // For select and PHI, find current ID according to Address and
        // ValueIDCandidates.
        // If two variables of select have the same value, we follow the one
        // occurs latter, this is just a temporary method.
        if (Record.PointeeAddress == CurrentState[PointerLabel].Address &&
            CurrentState[PointerLabel].ValueIDCandidates.count(
              Record.PointerValueID)) {
          CurrentState[PointerLabel].ValueIDCandidates.clear();
          CurrentState[PointerLabel].ValueID = Record.PointerValueID;

          NumContainingSlices++;
          CurrentState[PointerLabel].Trace.push_back(pair<unsigned, unsigned>(
            CurrentRecordID, CurrentState[PointerLabel].ValueID));
          trackSourcePointer(CurrentState[PointerLabel], Record);
        }
      } else {
        if (Record.PointerValueID == CurrentState[PointerLabel].ValueID) {
          NumContainingSlices++;
          CurrentState[PointerLabel].Trace.push_back(pair<unsigned, unsigned>(
            CurrentRecordID, CurrentState[PointerLabel].ValueID));
          trackSourcePointer(CurrentState[PointerLabel], Record);
        }
      }
    }
  }
  // If two sliced traces meet, we stop tracking
  if (NumContainingSlices == 2) {
    CurrentState[0].End = true;
    CurrentState[1].End = true;
  }
}

void TraceSlicer::trackSourcePointer(TraceState &TS,
                                    const TopLevelPointToLogRecord &Record) {
  IDAssigner &IDA = getAnalysis<IDAssigner>();
  Value *V = IDA.getValue(TS.ValueID);
  if (isa<LoadInst>(V)) {
    TS.Address = Record.LoadedFrom;
    TS.Action = AddrTakenPointTo;
  } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(V)) {
    TS.ValueID = IDA.getValueID(GEPI->getPointerOperand());
  } else if (BitCastInst *BCI = dyn_cast<BitCastInst>(V)) {
    TS.ValueID = IDA.getValueID(BCI->getOperand(0));
  } else if (SelectInst *SI = dyn_cast<SelectInst>(V)) {
    TS.ValueIDCandidates.insert(IDA.getValueID(SI->getTrueValue()));
    TS.ValueIDCandidates.insert(IDA.getValueID(SI->getFalseValue()));
    TS.Address = Record.PointeeAddress;
  } else if (PHINode *PN = dyn_cast<PHINode>(V)) {
    unsigned NumIncomingValues = PN->getNumIncomingValues();
    for (unsigned VI = 0; VI != NumIncomingValues; ++VI) {
      TS.ValueIDCandidates.insert(IDA.getValueID(PN->getIncomingValue(VI)));
    }
    TS.Address = Record.PointeeAddress;
  } else if (Argument *A = dyn_cast<Argument>(V)) {
    TS.Action = CallInstruction;
    TS.ArgNo = A->getArgNo();
    // errs() << "(argument of @" << A->getParent()->getName() << ")\n";
  } else if (isa<CallInst>(V) || isa<InvokeInst>(V)) {
    TS.Action = ReturnInstruction;
  } else if (isa<AllocaInst>(V)) {
    TS.End = true;
  } else if (isa<GlobalValue>(V)) {
    TS.End = true;
  } else {
    errs() << "Unknown instruction \'" << *V << "\'\n";
    TS.End = true;
  }
}

void TraceSlicer::processAddrTakenPointTo(
    const AddrTakenPointToLogRecord &Record) {
  CurrentRecordID--;
  int NumContainingSlices = 0;
  IDAssigner &IDA = getAnalysis<IDAssigner>();
  Instruction *I = IDA.getInstruction(Record.InstructionID);
  assert(isa<StoreInst>(I));

  for (int PointerLabel = 0; PointerLabel < 2; ++PointerLabel) {
    // Starting record must be a TopLevelPointTo record
    assert(CurrentState[PointerLabel].StartRecordID != CurrentRecordID);
    if (isLive(PointerLabel)) {
      if (CurrentState[PointerLabel].Action != AddrTakenPointTo) {
        continue;
      }
      if (Record.PointerAddress == CurrentState[PointerLabel].Address) {
        NumContainingSlices++;
        CurrentState[PointerLabel].Trace.push_back(pair<unsigned, unsigned>(
          CurrentRecordID, IDA.getValueID(I)));
        CurrentState[PointerLabel].Action = TopLevelPointTo;
        StoreInst *SI = dyn_cast<StoreInst>(I);
        CurrentState[PointerLabel].ValueID =
          IDA.getValueID(SI->getValueOperand());
      }
    }
  }
  // If two sliced traces meet, we stop tracking
  if (NumContainingSlices == 2) {
    CurrentState[0].End = true;
    CurrentState[1].End = true;
  }
}

void TraceSlicer::processCallInstruction(
    const CallInstructionLogRecord &Record) {
  CurrentRecordID--;
  int NumContainingSlices = 0;
  IDAssigner &IDA = getAnalysis<IDAssigner>();
  Instruction *I = IDA.getInstruction(Record.InstructionID);
  CallSite CS(I);
  assert(CS);

  for (int PointerLabel = 0; PointerLabel < 2; ++PointerLabel) {
    // Starting record must be a TopLevelPointTo record
    assert(CurrentState[PointerLabel].StartRecordID != CurrentRecordID);
    if (isLive(PointerLabel)) {
      if (CurrentState[PointerLabel].Action == ReturnInstruction) {
        // this callee is an external function, the trace ends
        CurrentState[PointerLabel].End = true;
        continue;
      }
      if (CurrentState[PointerLabel].Action != CallInstruction) {
        continue;
      }

      NumContainingSlices++;
      CurrentState[PointerLabel].Trace.push_back(pair<unsigned, unsigned>(
        CurrentRecordID, IDA.getValueID(I)));
      CurrentState[PointerLabel].Action = TopLevelPointTo;
      CurrentState[PointerLabel].ValueID =
        IDA.getValueID(CS.getArgument(CurrentState[PointerLabel].ArgNo));
    }
  }
  // If two sliced traces meet, we stop tracking
  if (NumContainingSlices == 2) {
    CurrentState[0].End = true;
    CurrentState[1].End = true;
  }
}

void TraceSlicer::processReturnInstruction(
    const ReturnInstructionLogRecord &Record) {
  CurrentRecordID--;
  int NumContainingSlices = 0;
  IDAssigner &IDA = getAnalysis<IDAssigner>();
  Instruction *I = IDA.getInstruction(Record.InstructionID);
  assert(isa<ReturnInst>(I) || isa<ResumeInst>(I));

  for (int PointerLabel = 0; PointerLabel < 2; ++PointerLabel) {
    // Starting record must be a TopLevelPointTo record
    assert(CurrentState[PointerLabel].StartRecordID != CurrentRecordID);
    if (isLive(PointerLabel)) {
      if (CurrentState[PointerLabel].Action != ReturnInstruction) {
        continue;
      }

      NumContainingSlices++;
      CurrentState[PointerLabel].Trace.push_back(pair<unsigned, unsigned>(
        CurrentRecordID, IDA.getValueID(I)));
      CurrentState[PointerLabel].Action = TopLevelPointTo;
      if (ReturnInst *RI = dyn_cast<ReturnInst>(I)) {
        CurrentState[PointerLabel].ValueID =
          IDA.getValueID(RI->getReturnValue());
      } else if (isa<ResumeInst>(I)) {
        assert(false);
      } else {
        assert(false);
      }
    }
  }
  // If two sliced traces meet, we stop tracking
  if (NumContainingSlices == 2) {
    CurrentState[0].End = true;
    CurrentState[1].End = true;
  }
}