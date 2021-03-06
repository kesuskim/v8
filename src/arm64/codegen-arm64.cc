// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/arm64/codegen-arm64.h"

#if V8_TARGET_ARCH_ARM64

#include "src/arm64/simulator-arm64.h"
#include "src/codegen.h"
#include "src/macro-assembler.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm)

UnaryMathFunctionWithIsolate CreateSqrtFunction(Isolate* isolate) {
  return nullptr;
}


// -------------------------------------------------------------------------
// Platform-specific RuntimeCallHelper functions.

void StubRuntimeCallHelper::BeforeCall(MacroAssembler* masm) const {
  masm->EnterFrame(StackFrame::INTERNAL);
  DCHECK(!masm->has_frame());
  masm->set_has_frame(true);
}


void StubRuntimeCallHelper::AfterCall(MacroAssembler* masm) const {
  masm->LeaveFrame(StackFrame::INTERNAL);
  DCHECK(masm->has_frame());
  masm->set_has_frame(false);
}


// -------------------------------------------------------------------------
// Code generators

CodeAgingHelper::CodeAgingHelper(Isolate* isolate) {
  USE(isolate);
  DCHECK(young_sequence_.length() == kNoCodeAgeSequenceLength);
  // The sequence of instructions that is patched out for aging code is the
  // following boilerplate stack-building prologue that is found both in
  // FUNCTION and OPTIMIZED_FUNCTION code:
  PatchingAssembler patcher(isolate, young_sequence_.start(),
                            young_sequence_.length() / kInstructionSize);
  // The young sequence is the frame setup code for FUNCTION code types. It is
  // generated by FullCodeGenerator::Generate.
  MacroAssembler::EmitFrameSetupForCodeAgePatching(&patcher);

#ifdef DEBUG
  const int length = kCodeAgeStubEntryOffset / kInstructionSize;
  DCHECK(old_sequence_.length() >= kCodeAgeStubEntryOffset);
  PatchingAssembler patcher_old(isolate, old_sequence_.start(), length);
  MacroAssembler::EmitCodeAgeSequence(&patcher_old, NULL);
#endif
}


#ifdef DEBUG
bool CodeAgingHelper::IsOld(byte* candidate) const {
  return memcmp(candidate, old_sequence_.start(), kCodeAgeStubEntryOffset) == 0;
}
#endif


bool Code::IsYoungSequence(Isolate* isolate, byte* sequence) {
  return MacroAssembler::IsYoungSequence(isolate, sequence);
}


void Code::GetCodeAgeAndParity(Isolate* isolate, byte* sequence, Age* age,
                               MarkingParity* parity) {
  if (IsYoungSequence(isolate, sequence)) {
    *age = kNoAgeCodeAge;
    *parity = NO_MARKING_PARITY;
  } else {
    byte* target = sequence + kCodeAgeStubEntryOffset;
    Code* stub = GetCodeFromTargetAddress(Memory::Address_at(target));
    GetCodeAgeAndParity(stub, age, parity);
  }
}


void Code::PatchPlatformCodeAge(Isolate* isolate,
                                byte* sequence,
                                Code::Age age,
                                MarkingParity parity) {
  PatchingAssembler patcher(isolate, sequence,
                            kNoCodeAgeSequenceLength / kInstructionSize);
  if (age == kNoAgeCodeAge) {
    MacroAssembler::EmitFrameSetupForCodeAgePatching(&patcher);
  } else {
    Code * stub = GetCodeAgeStub(isolate, age, parity);
    MacroAssembler::EmitCodeAgeSequence(&patcher, stub);
  }
}


void StringCharLoadGenerator::Generate(MacroAssembler* masm,
                                       Register string,
                                       Register index,
                                       Register result,
                                       Label* call_runtime) {
  DCHECK(string.Is64Bits() && index.Is32Bits() && result.Is64Bits());
  // Fetch the instance type of the receiver into result register.
  __ Ldr(result, FieldMemOperand(string, HeapObject::kMapOffset));
  __ Ldrb(result, FieldMemOperand(result, Map::kInstanceTypeOffset));

  // We need special handling for indirect strings.
  Label check_sequential;
  __ TestAndBranchIfAllClear(result, kIsIndirectStringMask, &check_sequential);

  // Dispatch on the indirect string shape: slice or cons.
  Label cons_string;
  __ TestAndBranchIfAllClear(result, kSlicedNotConsMask, &cons_string);

  // Handle slices.
  Label indirect_string_loaded;
  __ Ldr(result.W(),
         UntagSmiFieldMemOperand(string, SlicedString::kOffsetOffset));
  __ Ldr(string, FieldMemOperand(string, SlicedString::kParentOffset));
  __ Add(index, index, result.W());
  __ B(&indirect_string_loaded);

  // Handle cons strings.
  // Check whether the right hand side is the empty string (i.e. if
  // this is really a flat string in a cons string). If that is not
  // the case we would rather go to the runtime system now to flatten
  // the string.
  __ Bind(&cons_string);
  __ Ldr(result, FieldMemOperand(string, ConsString::kSecondOffset));
  __ JumpIfNotRoot(result, Heap::kempty_stringRootIndex, call_runtime);
  // Get the first of the two strings and load its instance type.
  __ Ldr(string, FieldMemOperand(string, ConsString::kFirstOffset));

  __ Bind(&indirect_string_loaded);
  __ Ldr(result, FieldMemOperand(string, HeapObject::kMapOffset));
  __ Ldrb(result, FieldMemOperand(result, Map::kInstanceTypeOffset));

  // Distinguish sequential and external strings. Only these two string
  // representations can reach here (slices and flat cons strings have been
  // reduced to the underlying sequential or external string).
  Label external_string, check_encoding;
  __ Bind(&check_sequential);
  STATIC_ASSERT(kSeqStringTag == 0);
  __ TestAndBranchIfAnySet(result, kStringRepresentationMask, &external_string);

  // Prepare sequential strings
  STATIC_ASSERT(SeqTwoByteString::kHeaderSize == SeqOneByteString::kHeaderSize);
  __ Add(string, string, SeqTwoByteString::kHeaderSize - kHeapObjectTag);
  __ B(&check_encoding);

  // Handle external strings.
  __ Bind(&external_string);
  if (FLAG_debug_code) {
    // Assert that we do not have a cons or slice (indirect strings) here.
    // Sequential strings have already been ruled out.
    __ Tst(result, kIsIndirectStringMask);
    __ Assert(eq, kExternalStringExpectedButNotFound);
  }
  // Rule out short external strings.
  STATIC_ASSERT(kShortExternalStringTag != 0);
  // TestAndBranchIfAnySet can emit Tbnz. Do not use it because call_runtime
  // can be bound far away in deferred code.
  __ Tst(result, kShortExternalStringMask);
  __ B(ne, call_runtime);
  __ Ldr(string, FieldMemOperand(string, ExternalString::kResourceDataOffset));

  Label one_byte, done;
  __ Bind(&check_encoding);
  STATIC_ASSERT(kTwoByteStringTag == 0);
  __ TestAndBranchIfAnySet(result, kStringEncodingMask, &one_byte);
  // Two-byte string.
  __ Ldrh(result, MemOperand(string, index, SXTW, 1));
  __ B(&done);
  __ Bind(&one_byte);
  // One-byte string.
  __ Ldrb(result, MemOperand(string, index, SXTW));
  __ Bind(&done);
}

#undef __

}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_ARM64
