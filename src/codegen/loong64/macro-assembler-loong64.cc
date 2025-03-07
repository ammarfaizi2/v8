// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>  // For LONG_MIN, LONG_MAX.

#if V8_TARGET_ARCH_LOONG64

#include "src/base/bits.h"
#include "src/base/division-by-constant.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/callable.h"
#include "src/codegen/code-factory.h"
#include "src/codegen/external-reference-table.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/register-configuration.h"
#include "src/debug/debug.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/frames-inl.h"
#include "src/heap/memory-chunk.h"
#include "src/init/bootstrapper.h"
#include "src/logging/counters.h"
#include "src/objects/heap-number.h"
#include "src/runtime/runtime.h"
#include "src/snapshot/snapshot.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/wasm-code-manager.h"
#endif  // V8_ENABLE_WEBASSEMBLY

// Satisfy cpplint check, but don't include platform-specific header. It is
// included recursively via macro-assembler.h.
#if 0
#include "src/codegen/loong64/macro-assembler-loong64.h"
#endif

namespace v8 {
namespace internal {

static inline bool IsZero(const Operand& rk) {
  if (rk.is_reg()) {
    return rk.rm() == zero_reg;
  } else {
    return rk.immediate() == 0;
  }
}

int TurboAssembler::RequiredStackSizeForCallerSaved(SaveFPRegsMode fp_mode,
                                                    Register exclusion1,
                                                    Register exclusion2,
                                                    Register exclusion3) const {
  int bytes = 0;
  RegList exclusions = 0;
  if (exclusion1 != no_reg) {
    exclusions |= exclusion1.bit();
    if (exclusion2 != no_reg) {
      exclusions |= exclusion2.bit();
      if (exclusion3 != no_reg) {
        exclusions |= exclusion3.bit();
      }
    }
  }

  RegList list = kJSCallerSaved & ~exclusions;
  bytes += NumRegs(list) * kPointerSize;

  if (fp_mode == SaveFPRegsMode::kSave) {
    bytes += NumRegs(kCallerSavedFPU) * kDoubleSize;
  }

  return bytes;
}

int TurboAssembler::PushCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1,
                                    Register exclusion2, Register exclusion3) {
  ASM_CODE_COMMENT(this);
  int bytes = 0;
  RegList exclusions = 0;
  if (exclusion1 != no_reg) {
    exclusions |= exclusion1.bit();
    if (exclusion2 != no_reg) {
      exclusions |= exclusion2.bit();
      if (exclusion3 != no_reg) {
        exclusions |= exclusion3.bit();
      }
    }
  }

  RegList list = kJSCallerSaved & ~exclusions;
  MultiPush(list);
  bytes += NumRegs(list) * kPointerSize;

  if (fp_mode == SaveFPRegsMode::kSave) {
    MultiPushFPU(kCallerSavedFPU);
    bytes += NumRegs(kCallerSavedFPU) * kDoubleSize;
  }

  return bytes;
}

int TurboAssembler::PopCallerSaved(SaveFPRegsMode fp_mode, Register exclusion1,
                                   Register exclusion2, Register exclusion3) {
  ASM_CODE_COMMENT(this);
  int bytes = 0;
  if (fp_mode == SaveFPRegsMode::kSave) {
    MultiPopFPU(kCallerSavedFPU);
    bytes += NumRegs(kCallerSavedFPU) * kDoubleSize;
  }

  RegList exclusions = 0;
  if (exclusion1 != no_reg) {
    exclusions |= exclusion1.bit();
    if (exclusion2 != no_reg) {
      exclusions |= exclusion2.bit();
      if (exclusion3 != no_reg) {
        exclusions |= exclusion3.bit();
      }
    }
  }

  RegList list = kJSCallerSaved & ~exclusions;
  MultiPop(list);
  bytes += NumRegs(list) * kPointerSize;

  return bytes;
}

void TurboAssembler::LoadRoot(Register destination, RootIndex index) {
  Ld_d(destination, MemOperand(s6, RootRegisterOffsetForRootIndex(index)));
}

void TurboAssembler::PushCommonFrame(Register marker_reg) {
  if (marker_reg.is_valid()) {
    Push(ra, fp, marker_reg);
    Add_d(fp, sp, Operand(kPointerSize));
  } else {
    Push(ra, fp);
    mov(fp, sp);
  }
}

void TurboAssembler::PushStandardFrame(Register function_reg) {
  int offset = -StandardFrameConstants::kContextOffset;
  if (function_reg.is_valid()) {
    Push(ra, fp, cp, function_reg, kJavaScriptCallArgCountRegister);
    offset += 2 * kPointerSize;
  } else {
    Push(ra, fp, cp, kJavaScriptCallArgCountRegister);
    offset += kPointerSize;
  }
  Add_d(fp, sp, Operand(offset));
}

// Clobbers object, dst, value, and ra, if (ra_status == kRAHasBeenSaved)
// The register 'object' contains a heap object pointer.  The heap object
// tag is shifted away.
void MacroAssembler::RecordWriteField(Register object, int offset,
                                      Register value, RAStatus ra_status,
                                      SaveFPRegsMode save_fp,
                                      RememberedSetAction remembered_set_action,
                                      SmiCheck smi_check) {
  ASM_CODE_COMMENT(this);
  // First, check if a write barrier is even needed. The tests below
  // catch stores of Smis.
  Label done;

  // Skip barrier if writing a smi.
  if (smi_check == SmiCheck::kInline) {
    JumpIfSmi(value, &done);
  }

  // Although the object register is tagged, the offset is relative to the start
  // of the object, so offset must be a multiple of kPointerSize.
  DCHECK(IsAligned(offset, kPointerSize));

  if (FLAG_debug_code) {
    Label ok;
    BlockTrampolinePoolScope block_trampoline_pool(this);
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    Add_d(scratch, object, offset - kHeapObjectTag);
    And(scratch, scratch, Operand(kPointerSize - 1));
    Branch(&ok, eq, scratch, Operand(zero_reg));
    Abort(AbortReason::kUnalignedCellInWriteBarrier);
    bind(&ok);
  }

  RecordWrite(object, Operand(offset - kHeapObjectTag), value, ra_status,
              save_fp, remembered_set_action, SmiCheck::kOmit);

  bind(&done);
}

void TurboAssembler::MaybeSaveRegisters(RegList registers) {
  if (registers == 0) return;
  RegList regs = 0;
  for (int i = 0; i < Register::kNumRegisters; ++i) {
    if ((registers >> i) & 1u) {
      regs |= Register::from_code(i).bit();
    }
  }
  MultiPush(regs);
}

void TurboAssembler::MaybeRestoreRegisters(RegList registers) {
  if (registers == 0) return;
  RegList regs = 0;
  for (int i = 0; i < Register::kNumRegisters; ++i) {
    if ((registers >> i) & 1u) {
      regs |= Register::from_code(i).bit();
    }
  }
  MultiPop(regs);
}

void TurboAssembler::CallEphemeronKeyBarrier(Register object, Operand offset,
                                             SaveFPRegsMode fp_mode) {
  ASM_CODE_COMMENT(this);
  RegList registers = WriteBarrierDescriptor::ComputeSavedRegisters(object);
  MaybeSaveRegisters(registers);

  Register object_parameter = WriteBarrierDescriptor::ObjectRegister();
  Register slot_address_parameter =
      WriteBarrierDescriptor::SlotAddressRegister();

  MoveObjectAndSlot(object_parameter, slot_address_parameter, object, offset);

  Call(isolate()->builtins()->code_handle(
           Builtins::GetEphemeronKeyBarrierStub(fp_mode)),
       RelocInfo::CODE_TARGET);
  MaybeRestoreRegisters(registers);
}

void TurboAssembler::CallRecordWriteStubSaveRegisters(
    Register object, Operand offset, RememberedSetAction remembered_set_action,
    SaveFPRegsMode fp_mode, StubCallMode mode) {
  ASM_CODE_COMMENT(this);
  RegList registers = WriteBarrierDescriptor::ComputeSavedRegisters(object);
  MaybeSaveRegisters(registers);

  Register object_parameter = WriteBarrierDescriptor::ObjectRegister();
  Register slot_address_parameter =
      WriteBarrierDescriptor::SlotAddressRegister();

  MoveObjectAndSlot(object_parameter, slot_address_parameter, object, offset);

  CallRecordWriteStub(object_parameter, slot_address_parameter,
                      remembered_set_action, fp_mode, mode);

  MaybeRestoreRegisters(registers);
}

void TurboAssembler::CallRecordWriteStub(
    Register object, Register slot_address,
    RememberedSetAction remembered_set_action, SaveFPRegsMode fp_mode,
    StubCallMode mode) {
  // Use CallRecordWriteStubSaveRegisters if the object and slot registers
  // need to be caller saved.
  DCHECK_EQ(WriteBarrierDescriptor::ObjectRegister(), object);
  DCHECK_EQ(WriteBarrierDescriptor::SlotAddressRegister(), slot_address);
#if V8_ENABLE_WEBASSEMBLY
  if (mode == StubCallMode::kCallWasmRuntimeStub) {
    auto wasm_target =
        wasm::WasmCode::GetRecordWriteStub(remembered_set_action, fp_mode);
    Call(wasm_target, RelocInfo::WASM_STUB_CALL);
#else
  if (false) {
#endif
  } else {
    auto builtin = Builtins::GetRecordWriteStub(remembered_set_action, fp_mode);
    if (options().inline_offheap_trampolines) {
      // Inline the trampoline.
      RecordCommentForOffHeapTrampoline(builtin);
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      li(scratch, Operand(BuiltinEntry(builtin), RelocInfo::OFF_HEAP_TARGET));
      Call(scratch);
      RecordComment("]");
    } else {
      Handle<Code> code_target = isolate()->builtins()->code_handle(builtin);
      Call(code_target, RelocInfo::CODE_TARGET);
    }
  }
}

void TurboAssembler::MoveObjectAndSlot(Register dst_object, Register dst_slot,
                                       Register object, Operand offset) {
  ASM_CODE_COMMENT(this);
  DCHECK_NE(dst_object, dst_slot);
  // If `offset` is a register, it cannot overlap with `object`.
  DCHECK_IMPLIES(!offset.IsImmediate(), offset.rm() != object);

  // If the slot register does not overlap with the object register, we can
  // overwrite it.
  if (dst_slot != object) {
    Add_d(dst_slot, object, offset);
    mov(dst_object, object);
    return;
  }

  DCHECK_EQ(dst_slot, object);

  // If the destination object register does not overlap with the offset
  // register, we can overwrite it.
  if (offset.IsImmediate() || (offset.rm() != dst_object)) {
    mov(dst_object, dst_slot);
    Add_d(dst_slot, dst_slot, offset);
    return;
  }

  DCHECK_EQ(dst_object, offset.rm());

  // We only have `dst_slot` and `dst_object` left as distinct registers so we
  // have to swap them. We write this as a add+sub sequence to avoid using a
  // scratch register.
  Add_d(dst_slot, dst_slot, dst_object);
  Sub_d(dst_object, dst_slot, dst_object);
}

// If lr_status is kLRHasBeenSaved, lr will be clobbered.
// TODO(LOONG_dev): LOONG64 Check this comment
// Clobbers object, address, value, and ra, if (ra_status == kRAHasBeenSaved)
// The register 'object' contains a heap object pointer.  The heap object
// tag is shifted away.
void MacroAssembler::RecordWrite(Register object, Operand offset,
                                 Register value, RAStatus ra_status,
                                 SaveFPRegsMode fp_mode,
                                 RememberedSetAction remembered_set_action,
                                 SmiCheck smi_check) {
  DCHECK(!AreAliased(object, value));

  if (FLAG_debug_code) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    Add_d(scratch, object, offset);
    Ld_d(scratch, MemOperand(scratch, 0));
    Assert(eq, AbortReason::kWrongAddressOrValuePassedToRecordWrite, scratch,
           Operand(value));
  }

  if ((remembered_set_action == RememberedSetAction::kOmit &&
       !FLAG_incremental_marking) ||
      FLAG_disable_write_barriers) {
    return;
  }

  // First, check if a write barrier is even needed. The tests below
  // catch stores of smis and stores into the young generation.
  Label done;

  if (smi_check == SmiCheck::kInline) {
    DCHECK_EQ(0, kSmiTag);
    JumpIfSmi(value, &done);
  }

  CheckPageFlag(value, MemoryChunk::kPointersToHereAreInterestingMask, eq,
                &done);

  CheckPageFlag(object, MemoryChunk::kPointersFromHereAreInterestingMask, eq,
                &done);

  // Record the actual write.
  if (ra_status == kRAHasNotBeenSaved) {
    Push(ra);
  }

  Register slot_address = WriteBarrierDescriptor::SlotAddressRegister();
  DCHECK(!AreAliased(object, slot_address, value));
  DCHECK(offset.IsImmediate());
  Add_d(slot_address, object, offset);
  CallRecordWriteStub(object, slot_address, remembered_set_action, fp_mode);
  if (ra_status == kRAHasNotBeenSaved) {
    Pop(ra);
  }

  bind(&done);
}

// ---------------------------------------------------------------------------
// Instruction macros.

void TurboAssembler::Add_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    add_w(rd, rj, rk.rm());
  } else {
    if (is_int12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      addi_w(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      DCHECK(rj != scratch);
      li(scratch, rk);
      add_w(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Add_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    add_d(rd, rj, rk.rm());
  } else {
    if (is_int12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      addi_d(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      DCHECK(rj != scratch);
      li(scratch, rk);
      add_d(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Sub_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    sub_w(rd, rj, rk.rm());
  } else {
    DCHECK(is_int32(rk.immediate()));
    if (is_int12(-rk.immediate()) && !MustUseReg(rk.rmode())) {
      // No subi_w instr, use addi_w(x, y, -imm).
      addi_w(rd, rj, static_cast<int32_t>(-rk.immediate()));
    } else {
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      DCHECK(rj != scratch);
      if (-rk.immediate() >> 12 == 0 && !MustUseReg(rk.rmode())) {
        // Use load -imm and addu when loading -imm generates one instruction.
        li(scratch, -rk.immediate());
        add_w(rd, rj, scratch);
      } else {
        // li handles the relocation.
        li(scratch, rk);
        sub_w(rd, rj, scratch);
      }
    }
  }
}

void TurboAssembler::Sub_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    sub_d(rd, rj, rk.rm());
  } else if (is_int12(-rk.immediate()) && !MustUseReg(rk.rmode())) {
    // No subi_d instr, use addi_d(x, y, -imm).
    addi_d(rd, rj, static_cast<int32_t>(-rk.immediate()));
  } else {
    DCHECK(rj != t7);
    int li_count = InstrCountForLi64Bit(rk.immediate());
    int li_neg_count = InstrCountForLi64Bit(-rk.immediate());
    if (li_neg_count < li_count && !MustUseReg(rk.rmode())) {
      // Use load -imm and add_d when loading -imm generates one instruction.
      DCHECK(rk.immediate() != std::numeric_limits<int32_t>::min());
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      li(scratch, Operand(-rk.immediate()));
      add_d(rd, rj, scratch);
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      li(scratch, rk);
      sub_d(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Mul_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mul_w(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mul_w(rd, rj, scratch);
  }
}

void TurboAssembler::Mulh_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mulh_w(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mulh_w(rd, rj, scratch);
  }
}

void TurboAssembler::Mulh_wu(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mulh_wu(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mulh_wu(rd, rj, scratch);
  }
}

void TurboAssembler::Mul_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mul_d(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mul_d(rd, rj, scratch);
  }
}

void TurboAssembler::Mulh_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mulh_d(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mulh_d(rd, rj, scratch);
  }
}

void TurboAssembler::Div_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    div_w(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    div_w(rd, rj, scratch);
  }
}

void TurboAssembler::Mod_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mod_w(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mod_w(rd, rj, scratch);
  }
}

void TurboAssembler::Mod_wu(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mod_wu(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mod_wu(rd, rj, scratch);
  }
}

void TurboAssembler::Div_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    div_d(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    div_d(rd, rj, scratch);
  }
}

void TurboAssembler::Div_wu(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    div_wu(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    div_wu(rd, rj, scratch);
  }
}

void TurboAssembler::Div_du(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    div_du(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    div_du(rd, rj, scratch);
  }
}

void TurboAssembler::Mod_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mod_d(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mod_d(rd, rj, scratch);
  }
}

void TurboAssembler::Mod_du(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    mod_du(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    mod_du(rd, rj, scratch);
  }
}

void TurboAssembler::And(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    and_(rd, rj, rk.rm());
  } else {
    if (is_uint12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      andi(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      DCHECK(rj != scratch);
      li(scratch, rk);
      and_(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Or(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    or_(rd, rj, rk.rm());
  } else {
    if (is_uint12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      ori(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      DCHECK(rj != scratch);
      li(scratch, rk);
      or_(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Xor(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    xor_(rd, rj, rk.rm());
  } else {
    if (is_uint12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      xori(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      Register scratch = temps.Acquire();
      DCHECK(rj != scratch);
      li(scratch, rk);
      xor_(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Nor(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    nor(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    nor(rd, rj, scratch);
  }
}

void TurboAssembler::Andn(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    andn(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    andn(rd, rj, scratch);
  }
}

void TurboAssembler::Orn(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    orn(rd, rj, rk.rm());
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    DCHECK(rj != scratch);
    li(scratch, rk);
    orn(rd, rj, scratch);
  }
}

void TurboAssembler::Neg(Register rj, const Operand& rk) {
  DCHECK(rk.is_reg());
  sub_d(rj, zero_reg, rk.rm());
}

void TurboAssembler::Slt(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    slt(rd, rj, rk.rm());
  } else {
    if (is_int12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      slti(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      BlockTrampolinePoolScope block_trampoline_pool(this);
      Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
      DCHECK(rj != scratch);
      li(scratch, rk);
      slt(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Sltu(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    sltu(rd, rj, rk.rm());
  } else {
    if (is_int12(rk.immediate()) && !MustUseReg(rk.rmode())) {
      sltui(rd, rj, static_cast<int32_t>(rk.immediate()));
    } else {
      // li handles the relocation.
      UseScratchRegisterScope temps(this);
      BlockTrampolinePoolScope block_trampoline_pool(this);
      Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
      DCHECK(rj != scratch);
      li(scratch, rk);
      sltu(rd, rj, scratch);
    }
  }
}

void TurboAssembler::Sle(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    slt(rd, rk.rm(), rj);
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
    BlockTrampolinePoolScope block_trampoline_pool(this);
    DCHECK(rj != scratch);
    li(scratch, rk);
    slt(rd, scratch, rj);
  }
  xori(rd, rd, 1);
}

void TurboAssembler::Sleu(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    sltu(rd, rk.rm(), rj);
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
    BlockTrampolinePoolScope block_trampoline_pool(this);
    DCHECK(rj != scratch);
    li(scratch, rk);
    sltu(rd, scratch, rj);
  }
  xori(rd, rd, 1);
}

void TurboAssembler::Sge(Register rd, Register rj, const Operand& rk) {
  Slt(rd, rj, rk);
  xori(rd, rd, 1);
}

void TurboAssembler::Sgeu(Register rd, Register rj, const Operand& rk) {
  Sltu(rd, rj, rk);
  xori(rd, rd, 1);
}

void TurboAssembler::Sgt(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    slt(rd, rk.rm(), rj);
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
    BlockTrampolinePoolScope block_trampoline_pool(this);
    DCHECK(rj != scratch);
    li(scratch, rk);
    slt(rd, scratch, rj);
  }
}

void TurboAssembler::Sgtu(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    sltu(rd, rk.rm(), rj);
  } else {
    // li handles the relocation.
    UseScratchRegisterScope temps(this);
    Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
    BlockTrampolinePoolScope block_trampoline_pool(this);
    DCHECK(rj != scratch);
    li(scratch, rk);
    sltu(rd, scratch, rj);
  }
}

void TurboAssembler::Rotr_w(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    rotr_w(rd, rj, rk.rm());
  } else {
    int64_t ror_value = rk.immediate() % 32;
    if (ror_value < 0) {
      ror_value += 32;
    }
    rotri_w(rd, rj, ror_value);
  }
}

void TurboAssembler::Rotr_d(Register rd, Register rj, const Operand& rk) {
  if (rk.is_reg()) {
    rotr_d(rd, rj, rk.rm());
  } else {
    int64_t dror_value = rk.immediate() % 64;
    if (dror_value < 0) dror_value += 64;
    rotri_d(rd, rj, dror_value);
  }
}

void TurboAssembler::Alsl_w(Register rd, Register rj, Register rk, uint8_t sa,
                            Register scratch) {
  DCHECK(sa >= 1 && sa <= 31);
  if (sa <= 4) {
    alsl_w(rd, rj, rk, sa);
  } else {
    Register tmp = rd == rk ? scratch : rd;
    DCHECK(tmp != rk);
    slli_w(tmp, rj, sa);
    add_w(rd, rk, tmp);
  }
}

void TurboAssembler::Alsl_d(Register rd, Register rj, Register rk, uint8_t sa,
                            Register scratch) {
  DCHECK(sa >= 1 && sa <= 63);
  if (sa <= 4) {
    alsl_d(rd, rj, rk, sa);
  } else {
    Register tmp = rd == rk ? scratch : rd;
    DCHECK(tmp != rk);
    slli_d(tmp, rj, sa);
    add_d(rd, rk, tmp);
  }
}

// ------------Pseudo-instructions-------------

// Change endianness
void TurboAssembler::ByteSwapSigned(Register dest, Register src,
                                    int operand_size) {
  DCHECK(operand_size == 2 || operand_size == 4 || operand_size == 8);
  if (operand_size == 2) {
    revb_2h(dest, src);
    ext_w_h(dest, dest);
  } else if (operand_size == 4) {
    revb_2w(dest, src);
    slli_w(dest, dest, 0);
  } else {
    revb_d(dest, dest);
  }
}

void TurboAssembler::ByteSwapUnsigned(Register dest, Register src,
                                      int operand_size) {
  DCHECK(operand_size == 2 || operand_size == 4);
  if (operand_size == 2) {
    revb_2h(dest, src);
    bstrins_d(dest, zero_reg, 63, 16);
  } else {
    revb_2w(dest, src);
    bstrins_d(dest, zero_reg, 63, 32);
  }
}

void TurboAssembler::Ld_b(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_b(rd, source.base(), source.index());
  } else {
    ld_b(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Ld_bu(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_bu(rd, source.base(), source.index());
  } else {
    ld_bu(rd, source.base(), source.offset());
  }
}

void TurboAssembler::St_b(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    stx_b(rd, source.base(), source.index());
  } else {
    st_b(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Ld_h(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_h(rd, source.base(), source.index());
  } else {
    ld_h(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Ld_hu(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_hu(rd, source.base(), source.index());
  } else {
    ld_hu(rd, source.base(), source.offset());
  }
}

void TurboAssembler::St_h(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    stx_h(rd, source.base(), source.index());
  } else {
    st_h(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Ld_w(Register rd, const MemOperand& rj) {
  MemOperand source = rj;

  if (!(source.hasIndexReg()) && is_int16(source.offset()) &&
      (source.offset() & 0b11) == 0) {
    ldptr_w(rd, source.base(), source.offset());
    return;
  }

  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_w(rd, source.base(), source.index());
  } else {
    ld_w(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Ld_wu(Register rd, const MemOperand& rj) {
  MemOperand source = rj;
  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_wu(rd, source.base(), source.index());
  } else {
    ld_wu(rd, source.base(), source.offset());
  }
}

void TurboAssembler::St_w(Register rd, const MemOperand& rj) {
  MemOperand source = rj;

  if (!(source.hasIndexReg()) && is_int16(source.offset()) &&
      (source.offset() & 0b11) == 0) {
    stptr_w(rd, source.base(), source.offset());
    return;
  }

  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    stx_w(rd, source.base(), source.index());
  } else {
    st_w(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Ld_d(Register rd, const MemOperand& rj) {
  MemOperand source = rj;

  if (!(source.hasIndexReg()) && is_int16(source.offset()) &&
      (source.offset() & 0b11) == 0) {
    ldptr_d(rd, source.base(), source.offset());
    return;
  }

  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    ldx_d(rd, source.base(), source.index());
  } else {
    ld_d(rd, source.base(), source.offset());
  }
}

void TurboAssembler::St_d(Register rd, const MemOperand& rj) {
  MemOperand source = rj;

  if (!(source.hasIndexReg()) && is_int16(source.offset()) &&
      (source.offset() & 0b11) == 0) {
    stptr_d(rd, source.base(), source.offset());
    return;
  }

  AdjustBaseAndOffset(&source);
  if (source.hasIndexReg()) {
    stx_d(rd, source.base(), source.index());
  } else {
    st_d(rd, source.base(), source.offset());
  }
}

void TurboAssembler::Fld_s(FPURegister fd, const MemOperand& src) {
  MemOperand tmp = src;
  AdjustBaseAndOffset(&tmp);
  if (tmp.hasIndexReg()) {
    fldx_s(fd, tmp.base(), tmp.index());
  } else {
    fld_s(fd, tmp.base(), tmp.offset());
  }
}

void TurboAssembler::Fst_s(FPURegister fs, const MemOperand& src) {
  MemOperand tmp = src;
  AdjustBaseAndOffset(&tmp);
  if (tmp.hasIndexReg()) {
    fstx_s(fs, tmp.base(), tmp.index());
  } else {
    fst_s(fs, tmp.base(), tmp.offset());
  }
}

void TurboAssembler::Fld_d(FPURegister fd, const MemOperand& src) {
  MemOperand tmp = src;
  AdjustBaseAndOffset(&tmp);
  if (tmp.hasIndexReg()) {
    fldx_d(fd, tmp.base(), tmp.index());
  } else {
    fld_d(fd, tmp.base(), tmp.offset());
  }
}

void TurboAssembler::Fst_d(FPURegister fs, const MemOperand& src) {
  MemOperand tmp = src;
  AdjustBaseAndOffset(&tmp);
  if (tmp.hasIndexReg()) {
    fstx_d(fs, tmp.base(), tmp.index());
  } else {
    fst_d(fs, tmp.base(), tmp.offset());
  }
}

void TurboAssembler::Ll_w(Register rd, const MemOperand& rj) {
  DCHECK(!rj.hasIndexReg());
  bool is_one_instruction = is_int14(rj.offset());
  if (is_one_instruction) {
    ll_w(rd, rj.base(), rj.offset());
  } else {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    li(scratch, rj.offset());
    add_d(scratch, scratch, rj.base());
    ll_w(rd, scratch, 0);
  }
}

void TurboAssembler::Ll_d(Register rd, const MemOperand& rj) {
  DCHECK(!rj.hasIndexReg());
  bool is_one_instruction = is_int14(rj.offset());
  if (is_one_instruction) {
    ll_d(rd, rj.base(), rj.offset());
  } else {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    li(scratch, rj.offset());
    add_d(scratch, scratch, rj.base());
    ll_d(rd, scratch, 0);
  }
}

void TurboAssembler::Sc_w(Register rd, const MemOperand& rj) {
  DCHECK(!rj.hasIndexReg());
  bool is_one_instruction = is_int14(rj.offset());
  if (is_one_instruction) {
    sc_w(rd, rj.base(), rj.offset());
  } else {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    li(scratch, rj.offset());
    add_d(scratch, scratch, rj.base());
    sc_w(rd, scratch, 0);
  }
}

void TurboAssembler::Sc_d(Register rd, const MemOperand& rj) {
  DCHECK(!rj.hasIndexReg());
  bool is_one_instruction = is_int14(rj.offset());
  if (is_one_instruction) {
    sc_d(rd, rj.base(), rj.offset());
  } else {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    li(scratch, rj.offset());
    add_d(scratch, scratch, rj.base());
    sc_d(rd, scratch, 0);
  }
}

void TurboAssembler::li(Register dst, Handle<HeapObject> value, LiFlags mode) {
  // TODO(jgruber,v8:8887): Also consider a root-relative load when generating
  // non-isolate-independent code. In many cases it might be cheaper than
  // embedding the relocatable value.
  if (root_array_available_ && options().isolate_independent_code) {
    IndirectLoadConstant(dst, value);
    return;
  }
  li(dst, Operand(value), mode);
}

void TurboAssembler::li(Register dst, ExternalReference value, LiFlags mode) {
  // TODO(jgruber,v8:8887): Also consider a root-relative load when generating
  // non-isolate-independent code. In many cases it might be cheaper than
  // embedding the relocatable value.
  if (root_array_available_ && options().isolate_independent_code) {
    IndirectLoadExternalReference(dst, value);
    return;
  }
  li(dst, Operand(value), mode);
}

void TurboAssembler::li(Register dst, const StringConstantBase* string,
                        LiFlags mode) {
  li(dst, Operand::EmbeddedStringConstant(string), mode);
}

static inline int InstrCountForLiLower32Bit(int64_t value) {
  if (is_int12(static_cast<int32_t>(value)) ||
      is_uint12(static_cast<int32_t>(value)) || !(value & kImm12Mask)) {
    return 1;
  } else {
    return 2;
  }
}

void TurboAssembler::LiLower32BitHelper(Register rd, Operand j) {
  if (is_int12(static_cast<int32_t>(j.immediate()))) {
    addi_d(rd, zero_reg, j.immediate());
  } else if (is_uint12(static_cast<int32_t>(j.immediate()))) {
    ori(rd, zero_reg, j.immediate() & kImm12Mask);
  } else {
    lu12i_w(rd, j.immediate() >> 12 & 0xfffff);
    if (j.immediate() & kImm12Mask) {
      ori(rd, rd, j.immediate() & kImm12Mask);
    }
  }
}

int TurboAssembler::InstrCountForLi64Bit(int64_t value) {
  if (is_int32(value)) {
    return InstrCountForLiLower32Bit(value);
  } else if (is_int52(value)) {
    return InstrCountForLiLower32Bit(value) + 1;
  } else if ((value & 0xffffffffL) == 0) {
    // 32 LSBs (Least Significant Bits) all set to zero.
    uint8_t tzc = base::bits::CountTrailingZeros32(value >> 32);
    uint8_t lzc = base::bits::CountLeadingZeros32(value >> 32);
    if (tzc >= 20) {
      return 1;
    } else if (tzc + lzc > 12) {
      return 2;
    } else {
      return 3;
    }
  } else {
    int64_t imm21 = (value >> 31) & 0x1fffffL;
    if (imm21 != 0x1fffffL && imm21 != 0) {
      return InstrCountForLiLower32Bit(value) + 2;
    } else {
      return InstrCountForLiLower32Bit(value) + 1;
    }
  }
  UNREACHABLE();
  return INT_MAX;
}

// All changes to if...else conditions here must be added to
// InstrCountForLi64Bit as well.
void TurboAssembler::li_optimized(Register rd, Operand j, LiFlags mode) {
  DCHECK(!j.is_reg());
  DCHECK(!MustUseReg(j.rmode()));
  DCHECK(mode == OPTIMIZE_SIZE);
  int64_t imm = j.immediate();
  BlockTrampolinePoolScope block_trampoline_pool(this);
  // Normal load of an immediate value which does not need Relocation Info.
  if (is_int32(imm)) {
    LiLower32BitHelper(rd, j);
  } else if (is_int52(imm)) {
    LiLower32BitHelper(rd, j);
    lu32i_d(rd, imm >> 32 & 0xfffff);
  } else if ((imm & 0xffffffffL) == 0) {
    // 32 LSBs (Least Significant Bits) all set to zero.
    uint8_t tzc = base::bits::CountTrailingZeros32(imm >> 32);
    uint8_t lzc = base::bits::CountLeadingZeros32(imm >> 32);
    if (tzc >= 20) {
      lu52i_d(rd, zero_reg, imm >> 52 & kImm12Mask);
    } else if (tzc + lzc > 12) {
      int32_t mask = (1 << (32 - tzc)) - 1;
      lu12i_w(rd, imm >> (tzc + 32) & mask);
      slli_d(rd, rd, tzc + 20);
    } else {
      xor_(rd, rd, rd);
      lu32i_d(rd, imm >> 32 & 0xfffff);
      lu52i_d(rd, rd, imm >> 52 & kImm12Mask);
    }
  } else {
    int64_t imm21 = (imm >> 31) & 0x1fffffL;
    LiLower32BitHelper(rd, j);
    if (imm21 != 0x1fffffL && imm21 != 0) lu32i_d(rd, imm >> 32 & 0xfffff);
    lu52i_d(rd, rd, imm >> 52 & kImm12Mask);
  }
}

void TurboAssembler::li(Register rd, Operand j, LiFlags mode) {
  DCHECK(!j.is_reg());
  BlockTrampolinePoolScope block_trampoline_pool(this);
  if (!MustUseReg(j.rmode()) && mode == OPTIMIZE_SIZE) {
    li_optimized(rd, j, mode);
  } else if (MustUseReg(j.rmode())) {
    int64_t immediate;
    if (j.IsHeapObjectRequest()) {
      RequestHeapObject(j.heap_object_request());
      immediate = 0;
    } else {
      immediate = j.immediate();
    }

    RecordRelocInfo(j.rmode(), immediate);
    lu12i_w(rd, immediate >> 12 & 0xfffff);
    ori(rd, rd, immediate & kImm12Mask);
    lu32i_d(rd, immediate >> 32 & 0xfffff);
  } else if (mode == ADDRESS_LOAD) {
    // We always need the same number of instructions as we may need to patch
    // this code to load another value which may need all 3 instructions.
    lu12i_w(rd, j.immediate() >> 12 & 0xfffff);
    ori(rd, rd, j.immediate() & kImm12Mask);
    lu32i_d(rd, j.immediate() >> 32 & 0xfffff);
  } else {  // mode == CONSTANT_SIZE - always emit the same instruction
            // sequence.
    lu12i_w(rd, j.immediate() >> 12 & 0xfffff);
    ori(rd, rd, j.immediate() & kImm12Mask);
    lu32i_d(rd, j.immediate() >> 32 & 0xfffff);
    lu52i_d(rd, rd, j.immediate() >> 52 & kImm12Mask);
  }
}

void TurboAssembler::MultiPush(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      St_d(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::MultiPush(RegList regs1, RegList regs2) {
  DCHECK_EQ(regs1 & regs2, 0);
  int16_t stack_offset = 0;

  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs1 & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      St_d(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs2 & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      St_d(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::MultiPush(RegList regs1, RegList regs2, RegList regs3) {
  DCHECK_EQ(regs1 & regs2, 0);
  DCHECK_EQ(regs1 & regs3, 0);
  DCHECK_EQ(regs2 & regs3, 0);
  int16_t stack_offset = 0;

  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs1 & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      St_d(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs2 & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      St_d(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs3 & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      St_d(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::MultiPop(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      Ld_d(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::MultiPop(RegList regs1, RegList regs2) {
  DCHECK_EQ(regs1 & regs2, 0);
  int16_t stack_offset = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs2 & (1 << i)) != 0) {
      Ld_d(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs1 & (1 << i)) != 0) {
      Ld_d(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::MultiPop(RegList regs1, RegList regs2, RegList regs3) {
  DCHECK_EQ(regs1 & regs2, 0);
  DCHECK_EQ(regs1 & regs3, 0);
  DCHECK_EQ(regs2 & regs3, 0);
  int16_t stack_offset = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs3 & (1 << i)) != 0) {
      Ld_d(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs2 & (1 << i)) != 0) {
      Ld_d(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs1 & (1 << i)) != 0) {
      Ld_d(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::MultiPushFPU(RegList regs) {
  int16_t num_to_push = base::bits::CountPopulation(regs);
  int16_t stack_offset = num_to_push * kDoubleSize;

  Sub_d(sp, sp, Operand(stack_offset));
  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs & (1 << i)) != 0) {
      stack_offset -= kDoubleSize;
      Fst_d(FPURegister::from_code(i), MemOperand(sp, stack_offset));
    }
  }
}

void TurboAssembler::MultiPopFPU(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      Fld_d(FPURegister::from_code(i), MemOperand(sp, stack_offset));
      stack_offset += kDoubleSize;
    }
  }
  addi_d(sp, sp, stack_offset);
}

void TurboAssembler::Bstrpick_w(Register rk, Register rj, uint16_t msbw,
                                uint16_t lsbw) {
  DCHECK_LT(lsbw, msbw);
  DCHECK_LT(lsbw, 32);
  DCHECK_LT(msbw, 32);
  bstrpick_w(rk, rj, msbw, lsbw);
}

void TurboAssembler::Bstrpick_d(Register rk, Register rj, uint16_t msbw,
                                uint16_t lsbw) {
  DCHECK_LT(lsbw, msbw);
  DCHECK_LT(lsbw, 64);
  DCHECK_LT(msbw, 64);
  bstrpick_d(rk, rj, msbw, lsbw);
}

void TurboAssembler::Neg_s(FPURegister fd, FPURegister fj) { fneg_s(fd, fj); }

void TurboAssembler::Neg_d(FPURegister fd, FPURegister fj) { fneg_d(fd, fj); }

void TurboAssembler::Ffint_d_uw(FPURegister fd, FPURegister fj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  movfr2gr_s(t8, fj);
  Ffint_d_uw(fd, t8);
}

void TurboAssembler::Ffint_d_uw(FPURegister fd, Register rj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  DCHECK(rj != t7);

  Bstrpick_d(t7, rj, 31, 0);
  movgr2fr_d(fd, t7);
  ffint_d_l(fd, fd);
}

void TurboAssembler::Ffint_d_ul(FPURegister fd, FPURegister fj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  movfr2gr_d(t8, fj);
  Ffint_d_ul(fd, t8);
}

void TurboAssembler::Ffint_d_ul(FPURegister fd, Register rj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  DCHECK(rj != t7);

  Label msb_clear, conversion_done;

  Branch(&msb_clear, ge, rj, Operand(zero_reg));

  // Rj >= 2^63
  andi(t7, rj, 1);
  srli_d(rj, rj, 1);
  or_(t7, t7, rj);
  movgr2fr_d(fd, t7);
  ffint_d_l(fd, fd);
  fadd_d(fd, fd, fd);
  Branch(&conversion_done);

  bind(&msb_clear);
  // Rs < 2^63, we can do simple conversion.
  movgr2fr_d(fd, rj);
  ffint_d_l(fd, fd);

  bind(&conversion_done);
}

void TurboAssembler::Ffint_s_uw(FPURegister fd, FPURegister fj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  movfr2gr_d(t8, fj);
  Ffint_s_uw(fd, t8);
}

void TurboAssembler::Ffint_s_uw(FPURegister fd, Register rj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  DCHECK(rj != t7);

  bstrpick_d(t7, rj, 31, 0);
  movgr2fr_d(fd, t7);
  ffint_s_l(fd, fd);
}

void TurboAssembler::Ffint_s_ul(FPURegister fd, FPURegister fj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  movfr2gr_d(t8, fj);
  Ffint_s_ul(fd, t8);
}

void TurboAssembler::Ffint_s_ul(FPURegister fd, Register rj) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  DCHECK(rj != t7);

  Label positive, conversion_done;

  Branch(&positive, ge, rj, Operand(zero_reg));

  // Rs >= 2^31.
  andi(t7, rj, 1);
  srli_d(rj, rj, 1);
  or_(t7, t7, rj);
  movgr2fr_d(fd, t7);
  ffint_s_l(fd, fd);
  fadd_s(fd, fd, fd);
  Branch(&conversion_done);

  bind(&positive);
  // Rs < 2^31, we can do simple conversion.
  movgr2fr_d(fd, rj);
  ffint_s_l(fd, fd);

  bind(&conversion_done);
}

void MacroAssembler::Ftintrne_l_d(FPURegister fd, FPURegister fj) {
  ftintrne_l_d(fd, fj);
}

void MacroAssembler::Ftintrm_l_d(FPURegister fd, FPURegister fj) {
  ftintrm_l_d(fd, fj);
}

void MacroAssembler::Ftintrp_l_d(FPURegister fd, FPURegister fj) {
  ftintrp_l_d(fd, fj);
}

void MacroAssembler::Ftintrz_l_d(FPURegister fd, FPURegister fj) {
  ftintrz_l_d(fd, fj);
}

void MacroAssembler::Ftintrz_l_ud(FPURegister fd, FPURegister fj,
                                  FPURegister scratch) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  // Load to GPR.
  movfr2gr_d(t8, fj);
  // Reset sign bit.
  {
    UseScratchRegisterScope temps(this);
    Register scratch1 = temps.Acquire();
    li(scratch1, 0x7FFFFFFFFFFFFFFFl);
    and_(t8, t8, scratch1);
  }
  movgr2fr_d(scratch, t8);
  Ftintrz_l_d(fd, scratch);
}

void TurboAssembler::Ftintrz_uw_d(FPURegister fd, FPURegister fj,
                                  FPURegister scratch) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Ftintrz_uw_d(t8, fj, scratch);
  movgr2fr_w(fd, t8);
}

void TurboAssembler::Ftintrz_uw_s(FPURegister fd, FPURegister fj,
                                  FPURegister scratch) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Ftintrz_uw_s(t8, fj, scratch);
  movgr2fr_w(fd, t8);
}

void TurboAssembler::Ftintrz_ul_d(FPURegister fd, FPURegister fj,
                                  FPURegister scratch, Register result) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Ftintrz_ul_d(t8, fj, scratch, result);
  movgr2fr_d(fd, t8);
}

void TurboAssembler::Ftintrz_ul_s(FPURegister fd, FPURegister fj,
                                  FPURegister scratch, Register result) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Ftintrz_ul_s(t8, fj, scratch, result);
  movgr2fr_d(fd, t8);
}

void MacroAssembler::Ftintrz_w_d(FPURegister fd, FPURegister fj) {
  ftintrz_w_d(fd, fj);
}

void MacroAssembler::Ftintrne_w_d(FPURegister fd, FPURegister fj) {
  ftintrne_w_d(fd, fj);
}

void MacroAssembler::Ftintrm_w_d(FPURegister fd, FPURegister fj) {
  ftintrm_w_d(fd, fj);
}

void MacroAssembler::Ftintrp_w_d(FPURegister fd, FPURegister fj) {
  ftintrp_w_d(fd, fj);
}

void TurboAssembler::Ftintrz_uw_d(Register rd, FPURegister fj,
                                  FPURegister scratch) {
  DCHECK(fj != scratch);
  DCHECK(rd != t7);

  {
    // Load 2^31 into scratch as its float representation.
    UseScratchRegisterScope temps(this);
    Register scratch1 = temps.Acquire();
    li(scratch1, 0x41E00000);
    movgr2fr_w(scratch, zero_reg);
    movgr2frh_w(scratch, scratch1);
  }
  // Test if scratch > fd.
  // If fd < 2^31 we can convert it normally.
  Label simple_convert;
  CompareF64(fj, scratch, CLT);
  BranchTrueShortF(&simple_convert);

  // First we subtract 2^31 from fd, then trunc it to rs
  // and add 2^31 to rj.
  fsub_d(scratch, fj, scratch);
  ftintrz_w_d(scratch, scratch);
  movfr2gr_s(rd, scratch);
  Or(rd, rd, 1 << 31);

  Label done;
  Branch(&done);
  // Simple conversion.
  bind(&simple_convert);
  ftintrz_w_d(scratch, fj);
  movfr2gr_s(rd, scratch);

  bind(&done);
}

void TurboAssembler::Ftintrz_uw_s(Register rd, FPURegister fj,
                                  FPURegister scratch) {
  DCHECK(fj != scratch);
  DCHECK(rd != t7);
  {
    // Load 2^31 into scratch as its float representation.
    UseScratchRegisterScope temps(this);
    Register scratch1 = temps.Acquire();
    li(scratch1, 0x4F000000);
    movgr2fr_w(scratch, scratch1);
  }
  // Test if scratch > fs.
  // If fs < 2^31 we can convert it normally.
  Label simple_convert;
  CompareF32(fj, scratch, CLT);
  BranchTrueShortF(&simple_convert);

  // First we subtract 2^31 from fs, then trunc it to rd
  // and add 2^31 to rd.
  fsub_s(scratch, fj, scratch);
  ftintrz_w_s(scratch, scratch);
  movfr2gr_s(rd, scratch);
  Or(rd, rd, 1 << 31);

  Label done;
  Branch(&done);
  // Simple conversion.
  bind(&simple_convert);
  ftintrz_w_s(scratch, fj);
  movfr2gr_s(rd, scratch);

  bind(&done);
}

void TurboAssembler::Ftintrz_ul_d(Register rd, FPURegister fj,
                                  FPURegister scratch, Register result) {
  DCHECK(fj != scratch);
  DCHECK(result.is_valid() ? !AreAliased(rd, result, t7) : !AreAliased(rd, t7));

  Label simple_convert, done, fail;
  if (result.is_valid()) {
    mov(result, zero_reg);
    Move(scratch, -1.0);
    // If fd =< -1 or unordered, then the conversion fails.
    CompareF64(fj, scratch, CLE);
    BranchTrueShortF(&fail);
    CompareIsNanF64(fj, scratch);
    BranchTrueShortF(&fail);
  }

  // Load 2^63 into scratch as its double representation.
  li(t7, 0x43E0000000000000);
  movgr2fr_d(scratch, t7);

  // Test if scratch > fs.
  // If fs < 2^63 we can convert it normally.
  CompareF64(fj, scratch, CLT);
  BranchTrueShortF(&simple_convert);

  // First we subtract 2^63 from fs, then trunc it to rd
  // and add 2^63 to rd.
  fsub_d(scratch, fj, scratch);
  ftintrz_l_d(scratch, scratch);
  movfr2gr_d(rd, scratch);
  Or(rd, rd, Operand(1UL << 63));
  Branch(&done);

  // Simple conversion.
  bind(&simple_convert);
  ftintrz_l_d(scratch, fj);
  movfr2gr_d(rd, scratch);

  bind(&done);
  if (result.is_valid()) {
    // Conversion is failed if the result is negative.
    {
      UseScratchRegisterScope temps(this);
      Register scratch1 = temps.Acquire();
      addi_d(scratch1, zero_reg, -1);
      srli_d(scratch1, scratch1, 1);  // Load 2^62.
      movfr2gr_d(result, scratch);
      xor_(result, result, scratch1);
    }
    Slt(result, zero_reg, result);
  }

  bind(&fail);
}

void TurboAssembler::Ftintrz_ul_s(Register rd, FPURegister fj,
                                  FPURegister scratch, Register result) {
  DCHECK(fj != scratch);
  DCHECK(result.is_valid() ? !AreAliased(rd, result, t7) : !AreAliased(rd, t7));

  Label simple_convert, done, fail;
  if (result.is_valid()) {
    mov(result, zero_reg);
    Move(scratch, -1.0f);
    // If fd =< -1 or unordered, then the conversion fails.
    CompareF32(fj, scratch, CLE);
    BranchTrueShortF(&fail);
    CompareIsNanF32(fj, scratch);
    BranchTrueShortF(&fail);
  }

  {
    // Load 2^63 into scratch as its float representation.
    UseScratchRegisterScope temps(this);
    Register scratch1 = temps.Acquire();
    li(scratch1, 0x5F000000);
    movgr2fr_w(scratch, scratch1);
  }

  // Test if scratch > fs.
  // If fs < 2^63 we can convert it normally.
  CompareF32(fj, scratch, CLT);
  BranchTrueShortF(&simple_convert);

  // First we subtract 2^63 from fs, then trunc it to rd
  // and add 2^63 to rd.
  fsub_s(scratch, fj, scratch);
  ftintrz_l_s(scratch, scratch);
  movfr2gr_d(rd, scratch);
  Or(rd, rd, Operand(1UL << 63));
  Branch(&done);

  // Simple conversion.
  bind(&simple_convert);
  ftintrz_l_s(scratch, fj);
  movfr2gr_d(rd, scratch);

  bind(&done);
  if (result.is_valid()) {
    // Conversion is failed if the result is negative or unordered.
    {
      UseScratchRegisterScope temps(this);
      Register scratch1 = temps.Acquire();
      addi_d(scratch1, zero_reg, -1);
      srli_d(scratch1, scratch1, 1);  // Load 2^62.
      movfr2gr_d(result, scratch);
      xor_(result, result, scratch1);
    }
    Slt(result, zero_reg, result);
  }

  bind(&fail);
}

void TurboAssembler::RoundDouble(FPURegister dst, FPURegister src,
                                 FPURoundingMode mode) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Register scratch = t8;
  movfcsr2gr(scratch);
  li(t7, Operand(mode));
  movgr2fcsr(t7);
  frint_d(dst, src);
  movgr2fcsr(scratch);
}

void TurboAssembler::Floor_d(FPURegister dst, FPURegister src) {
  RoundDouble(dst, src, mode_floor);
}

void TurboAssembler::Ceil_d(FPURegister dst, FPURegister src) {
  RoundDouble(dst, src, mode_ceil);
}

void TurboAssembler::Trunc_d(FPURegister dst, FPURegister src) {
  RoundDouble(dst, src, mode_trunc);
}

void TurboAssembler::Round_d(FPURegister dst, FPURegister src) {
  RoundDouble(dst, src, mode_round);
}

void TurboAssembler::RoundFloat(FPURegister dst, FPURegister src,
                                FPURoundingMode mode) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Register scratch = t8;
  movfcsr2gr(scratch);
  li(t7, Operand(mode));
  movgr2fcsr(t7);
  frint_s(dst, src);
  movgr2fcsr(scratch);
}

void TurboAssembler::Floor_s(FPURegister dst, FPURegister src) {
  RoundFloat(dst, src, mode_floor);
}

void TurboAssembler::Ceil_s(FPURegister dst, FPURegister src) {
  RoundFloat(dst, src, mode_ceil);
}

void TurboAssembler::Trunc_s(FPURegister dst, FPURegister src) {
  RoundFloat(dst, src, mode_trunc);
}

void TurboAssembler::Round_s(FPURegister dst, FPURegister src) {
  RoundFloat(dst, src, mode_round);
}

void TurboAssembler::CompareF(FPURegister cmp1, FPURegister cmp2,
                              FPUCondition cc, CFRegister cd, bool f32) {
  if (f32) {
    fcmp_cond_s(cc, cmp1, cmp2, cd);
  } else {
    fcmp_cond_d(cc, cmp1, cmp2, cd);
  }
}

void TurboAssembler::CompareIsNanF(FPURegister cmp1, FPURegister cmp2,
                                   CFRegister cd, bool f32) {
  CompareF(cmp1, cmp2, CUN, cd, f32);
}

void TurboAssembler::BranchTrueShortF(Label* target, CFRegister cj) {
  bcnez(cj, target);
}

void TurboAssembler::BranchFalseShortF(Label* target, CFRegister cj) {
  bceqz(cj, target);
}

void TurboAssembler::BranchTrueF(Label* target, CFRegister cj) {
  // TODO(yuyin): can be optimzed
  bool long_branch = target->is_bound()
                         ? !is_near(target, OffsetSize::kOffset21)
                         : is_trampoline_emitted();
  if (long_branch) {
    Label skip;
    BranchFalseShortF(&skip, cj);
    Branch(target);
    bind(&skip);
  } else {
    BranchTrueShortF(target, cj);
  }
}

void TurboAssembler::BranchFalseF(Label* target, CFRegister cj) {
  bool long_branch = target->is_bound()
                         ? !is_near(target, OffsetSize::kOffset21)
                         : is_trampoline_emitted();
  if (long_branch) {
    Label skip;
    BranchTrueShortF(&skip, cj);
    Branch(target);
    bind(&skip);
  } else {
    BranchFalseShortF(target, cj);
  }
}

void TurboAssembler::FmoveLow(FPURegister dst, Register src_low) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  DCHECK(src_low != scratch);
  movfrh2gr_s(scratch, dst);
  movgr2fr_w(dst, src_low);
  movgr2frh_w(dst, scratch);
}

void TurboAssembler::Move(FPURegister dst, uint32_t src) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  li(scratch, Operand(static_cast<int32_t>(src)));
  movgr2fr_w(dst, scratch);
}

void TurboAssembler::Move(FPURegister dst, uint64_t src) {
  // Handle special values first.
  if (src == bit_cast<uint64_t>(0.0) && has_double_zero_reg_set_) {
    fmov_d(dst, kDoubleRegZero);
  } else if (src == bit_cast<uint64_t>(-0.0) && has_double_zero_reg_set_) {
    Neg_d(dst, kDoubleRegZero);
  } else {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    li(scratch, Operand(static_cast<int64_t>(src)));
    movgr2fr_d(dst, scratch);
    if (dst == kDoubleRegZero) has_double_zero_reg_set_ = true;
  }
}

void TurboAssembler::Movz(Register rd, Register rj, Register rk) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  masknez(scratch, rj, rk);
  maskeqz(rd, rd, rk);
  or_(rd, rd, scratch);
}

void TurboAssembler::Movn(Register rd, Register rj, Register rk) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  maskeqz(scratch, rj, rk);
  masknez(rd, rd, rk);
  or_(rd, rd, scratch);
}

void TurboAssembler::LoadZeroOnCondition(Register rd, Register rj,
                                         const Operand& rk, Condition cond) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  switch (cond) {
    case cc_always:
      mov(rd, zero_reg);
      break;
    case eq:
      if (rj == zero_reg) {
        if (rk.is_reg()) {
          LoadZeroIfConditionZero(rd, rk.rm());
        } else if (rk.immediate() == 0) {
          mov(rd, zero_reg);
        }
      } else if (IsZero(rk)) {
        LoadZeroIfConditionZero(rd, rj);
      } else {
        Sub_d(t7, rj, rk);
        LoadZeroIfConditionZero(rd, t7);
      }
      break;
    case ne:
      if (rj == zero_reg) {
        if (rk.is_reg()) {
          LoadZeroIfConditionNotZero(rd, rk.rm());
        } else if (rk.immediate() != 0) {
          mov(rd, zero_reg);
        }
      } else if (IsZero(rk)) {
        LoadZeroIfConditionNotZero(rd, rj);
      } else {
        Sub_d(t7, rj, rk);
        LoadZeroIfConditionNotZero(rd, t7);
      }
      break;

    // Signed comparison.
    case greater:
      Sgt(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      break;
    case greater_equal:
      Sge(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj >= rk
      break;
    case less:
      Slt(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj < rk
      break;
    case less_equal:
      Sle(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj <= rk
      break;

    // Unsigned comparison.
    case Ugreater:
      Sgtu(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj > rk
      break;

    case Ugreater_equal:
      Sgeu(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj >= rk
      break;
    case Uless:
      Sltu(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj < rk
      break;
    case Uless_equal:
      Sleu(t7, rj, rk);
      LoadZeroIfConditionNotZero(rd, t7);
      // rj <= rk
      break;
    default:
      UNREACHABLE();
  }  // namespace internal
}  // namespace internal

void TurboAssembler::LoadZeroIfConditionNotZero(Register dest,
                                                Register condition) {
  masknez(dest, dest, condition);
}

void TurboAssembler::LoadZeroIfConditionZero(Register dest,
                                             Register condition) {
  maskeqz(dest, dest, condition);
}

void TurboAssembler::LoadZeroIfFPUCondition(Register dest, CFRegister cc) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  movcf2gr(scratch, cc);
  LoadZeroIfConditionNotZero(dest, scratch);
}

void TurboAssembler::LoadZeroIfNotFPUCondition(Register dest, CFRegister cc) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  movcf2gr(scratch, cc);
  LoadZeroIfConditionZero(dest, scratch);
}

void TurboAssembler::Clz_w(Register rd, Register rj) { clz_w(rd, rj); }

void TurboAssembler::Clz_d(Register rd, Register rj) { clz_d(rd, rj); }

void TurboAssembler::Ctz_w(Register rd, Register rj) { ctz_w(rd, rj); }

void TurboAssembler::Ctz_d(Register rd, Register rj) { ctz_d(rd, rj); }

// TODO(LOONG_dev): Optimize like arm64, use simd instruction
void TurboAssembler::Popcnt_w(Register rd, Register rj) {
  ASM_CODE_COMMENT(this);
  // https://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
  //
  // A generalization of the best bit counting method to integers of
  // bit-widths up to 128 (parameterized by type T) is this:
  //
  // v = v - ((v >> 1) & (T)~(T)0/3);                           // temp
  // v = (v & (T)~(T)0/15*3) + ((v >> 2) & (T)~(T)0/15*3);      // temp
  // v = (v + (v >> 4)) & (T)~(T)0/255*15;                      // temp
  // c = (T)(v * ((T)~(T)0/255)) >> (sizeof(T) - 1) * BITS_PER_BYTE; //count
  //
  // There are algorithms which are faster in the cases where very few
  // bits are set but the algorithm here attempts to minimize the total
  // number of instructions executed even when a large number of bits
  // are set.
  int32_t B0 = 0x55555555;     // (T)~(T)0/3
  int32_t B1 = 0x33333333;     // (T)~(T)0/15*3
  int32_t B2 = 0x0F0F0F0F;     // (T)~(T)0/255*15
  int32_t value = 0x01010101;  // (T)~(T)0/255
  uint32_t shift = 24;         // (sizeof(T) - 1) * BITS_PER_BYTE

  UseScratchRegisterScope temps(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Register scratch = temps.Acquire();
  Register scratch2 = t8;
  srli_w(scratch, rj, 1);
  li(scratch2, B0);
  And(scratch, scratch, scratch2);
  Sub_w(scratch, rj, scratch);
  li(scratch2, B1);
  And(rd, scratch, scratch2);
  srli_w(scratch, scratch, 2);
  And(scratch, scratch, scratch2);
  Add_w(scratch, rd, scratch);
  srli_w(rd, scratch, 4);
  Add_w(rd, rd, scratch);
  li(scratch2, B2);
  And(rd, rd, scratch2);
  li(scratch, value);
  Mul_w(rd, rd, scratch);
  srli_w(rd, rd, shift);
}

void TurboAssembler::Popcnt_d(Register rd, Register rj) {
  ASM_CODE_COMMENT(this);
  int64_t B0 = 0x5555555555555555l;     // (T)~(T)0/3
  int64_t B1 = 0x3333333333333333l;     // (T)~(T)0/15*3
  int64_t B2 = 0x0F0F0F0F0F0F0F0Fl;     // (T)~(T)0/255*15
  int64_t value = 0x0101010101010101l;  // (T)~(T)0/255
  uint32_t shift = 56;                  // (sizeof(T) - 1) * BITS_PER_BYTE

  UseScratchRegisterScope temps(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Register scratch = temps.Acquire();
  Register scratch2 = t8;
  srli_d(scratch, rj, 1);
  li(scratch2, B0);
  And(scratch, scratch, scratch2);
  Sub_d(scratch, rj, scratch);
  li(scratch2, B1);
  And(rd, scratch, scratch2);
  srli_d(scratch, scratch, 2);
  And(scratch, scratch, scratch2);
  Add_d(scratch, rd, scratch);
  srli_d(rd, scratch, 4);
  Add_d(rd, rd, scratch);
  li(scratch2, B2);
  And(rd, rd, scratch2);
  li(scratch, value);
  Mul_d(rd, rd, scratch);
  srli_d(rd, rd, shift);
}

void TurboAssembler::ExtractBits(Register dest, Register source, Register pos,
                                 int size, bool sign_extend) {
  sra_d(dest, source, pos);
  bstrpick_d(dest, dest, size - 1, 0);
  if (sign_extend) {
    switch (size) {
      case 8:
        ext_w_b(dest, dest);
        break;
      case 16:
        ext_w_h(dest, dest);
        break;
      case 32:
        // sign-extend word
        slli_w(dest, dest, 0);
        break;
      default:
        UNREACHABLE();
    }
  }
}

void TurboAssembler::InsertBits(Register dest, Register source, Register pos,
                                int size) {
  Rotr_d(dest, dest, pos);
  bstrins_d(dest, source, size - 1, 0);
  {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    Sub_d(scratch, zero_reg, pos);
    Rotr_d(dest, dest, scratch);
  }
}

void TurboAssembler::TryInlineTruncateDoubleToI(Register result,
                                                DoubleRegister double_input,
                                                Label* done) {
  DoubleRegister single_scratch = kScratchDoubleReg.low();
  BlockTrampolinePoolScope block_trampoline_pool(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Register scratch2 = temps.Acquire();

  ftintrz_l_d(single_scratch, double_input);
  movfr2gr_d(scratch2, single_scratch);
  li(scratch, 1L << 63);
  Xor(scratch, scratch, scratch2);
  rotri_d(scratch2, scratch, 1);
  movfr2gr_s(result, single_scratch);
  Branch(done, ne, scratch, Operand(scratch2));

  // Truncate NaN to zero.
  CompareIsNanF64(double_input, double_input);
  Move(result, zero_reg);
  bcnez(FCC0, done);
}

void TurboAssembler::TruncateDoubleToI(Isolate* isolate, Zone* zone,
                                       Register result,
                                       DoubleRegister double_input,
                                       StubCallMode stub_mode) {
  Label done;

  TryInlineTruncateDoubleToI(result, double_input, &done);

  // If we fell through then inline version didn't succeed - call stub instead.
  Sub_d(sp, sp,
        Operand(kDoubleSize + kSystemPointerSize));  // Put input on stack.
  St_d(ra, MemOperand(sp, kSystemPointerSize));
  Fst_d(double_input, MemOperand(sp, 0));

#if V8_ENABLE_WEBASSEMBLY
  if (stub_mode == StubCallMode::kCallWasmRuntimeStub) {
    Call(wasm::WasmCode::kDoubleToI, RelocInfo::WASM_STUB_CALL);
#else
  // For balance.
  if (false) {
#endif  // V8_ENABLE_WEBASSEMBLY
  } else {
    Call(BUILTIN_CODE(isolate, DoubleToI), RelocInfo::CODE_TARGET);
  }

  Pop(ra, result);
  bind(&done);
}

// BRANCH_ARGS_CHECK checks that conditional jump arguments are correct.
#define BRANCH_ARGS_CHECK(cond, rj, rk)                                  \
  DCHECK((cond == cc_always && rj == zero_reg && rk.rm() == zero_reg) || \
         (cond != cc_always && (rj != zero_reg || rk.rm() != zero_reg)))

void TurboAssembler::Branch(Label* L, bool need_link) {
  int offset = GetOffset(L, OffsetSize::kOffset26);
  if (need_link) {
    bl(offset);
  } else {
    b(offset);
  }
}

void TurboAssembler::Branch(Label* L, Condition cond, Register rj,
                            const Operand& rk, bool need_link) {
  if (L->is_bound()) {
    BRANCH_ARGS_CHECK(cond, rj, rk);
    if (!BranchShortOrFallback(L, cond, rj, rk, need_link)) {
      if (cond != cc_always) {
        Label skip;
        Condition neg_cond = NegateCondition(cond);
        BranchShort(&skip, neg_cond, rj, rk, need_link);
        Branch(L, need_link);
        bind(&skip);
      } else {
        Branch(L);
      }
    }
  } else {
    if (is_trampoline_emitted()) {
      if (cond != cc_always) {
        Label skip;
        Condition neg_cond = NegateCondition(cond);
        BranchShort(&skip, neg_cond, rj, rk, need_link);
        Branch(L, need_link);
        bind(&skip);
      } else {
        Branch(L);
      }
    } else {
      BranchShort(L, cond, rj, rk, need_link);
    }
  }
}

void TurboAssembler::Branch(Label* L, Condition cond, Register rj,
                            RootIndex index) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  LoadRoot(scratch, index);
  Branch(L, cond, rj, Operand(scratch));
}

int32_t TurboAssembler::GetOffset(Label* L, OffsetSize bits) {
  return branch_offset_helper(L, bits) >> 2;
}

Register TurboAssembler::GetRkAsRegisterHelper(const Operand& rk,
                                               Register scratch) {
  Register r2 = no_reg;
  if (rk.is_reg()) {
    r2 = rk.rm();
  } else {
    r2 = scratch;
    li(r2, rk);
  }

  return r2;
}

bool TurboAssembler::BranchShortOrFallback(Label* L, Condition cond,
                                           Register rj, const Operand& rk,
                                           bool need_link) {
  UseScratchRegisterScope temps(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Register scratch = temps.hasAvailable() ? temps.Acquire() : t8;
  DCHECK_NE(rj, zero_reg);

  // Be careful to always use shifted_branch_offset only just before the
  // branch instruction, as the location will be remember for patching the
  // target.
  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    int offset = 0;
    switch (cond) {
      case cc_always:
        if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
        offset = GetOffset(L, OffsetSize::kOffset26);
        if (need_link) {
          bl(offset);
        } else {
          b(offset);
        }
        break;
      case eq:
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          // beq is used here to make the code patchable. Otherwise b should
          // be used which has no condition field so is not patchable.
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset16);
          beq(rj, rj, offset);
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset21)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset21);
          beqz(rj, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          // We don't want any other register but scratch clobbered.
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          offset = GetOffset(L, OffsetSize::kOffset16);
          beq(rj, sc, offset);
        }
        break;
      case ne:
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          // bne is used here to make the code patchable. Otherwise we
          // should not generate any instruction.
          offset = GetOffset(L, OffsetSize::kOffset16);
          bne(rj, rj, offset);
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset21)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset21);
          bnez(rj, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          // We don't want any other register but scratch clobbered.
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bne(rj, sc, offset);
        }
        break;

      // Signed comparison.
      case greater:
        // rj > rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          // No code needs to be emitted.
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset16);
          blt(zero_reg, rj, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          blt(sc, rj, offset);
        }
        break;
      case greater_equal:
        // rj >= rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset26);
          b(offset);
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bge(rj, zero_reg, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bge(rj, sc, offset);
        }
        break;
      case less:
        // rj < rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          // No code needs to be emitted.
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset16);
          blt(rj, zero_reg, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          blt(rj, sc, offset);
        }
        break;
      case less_equal:
        // rj <= rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset26);
          b(offset);
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bge(zero_reg, rj, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bge(sc, rj, offset);
        }
        break;

      // Unsigned comparison.
      case Ugreater:
        // rj > rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          // No code needs to be emitted.
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset26);
          bnez(rj, offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bltu(sc, rj, offset);
        }
        break;
      case Ugreater_equal:
        // rj >= rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset26);
          b(offset);
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset26);
          b(offset);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bgeu(rj, sc, offset);
        }
        break;
      case Uless:
        // rj < rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          // No code needs to be emitted.
        } else if (IsZero(rk)) {
          // No code needs to be emitted.
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bltu(rj, sc, offset);
        }
        break;
      case Uless_equal:
        // rj <= rk
        if (rk.is_reg() && rj.code() == rk.rm().code()) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset26)) return false;
          if (need_link) pcaddi(ra, 2);
          offset = GetOffset(L, OffsetSize::kOffset26);
          b(offset);
        } else if (IsZero(rk)) {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset21)) return false;
          if (need_link) pcaddi(ra, 2);
          beqz(rj, L);
        } else {
          if (L->is_bound() && !is_near(L, OffsetSize::kOffset16)) return false;
          if (need_link) pcaddi(ra, 2);
          Register sc = GetRkAsRegisterHelper(rk, scratch);
          DCHECK(rj != sc);
          offset = GetOffset(L, OffsetSize::kOffset16);
          bgeu(sc, rj, offset);
        }
        break;
      default:
        UNREACHABLE();
    }
  }
  return true;
}

void TurboAssembler::BranchShort(Label* L, Condition cond, Register rj,
                                 const Operand& rk, bool need_link) {
  BRANCH_ARGS_CHECK(cond, rj, rk);
  bool result = BranchShortOrFallback(L, cond, rj, rk, need_link);
  DCHECK(result);
  USE(result);
}

void TurboAssembler::LoadFromConstantsTable(Register destination,
                                            int constant_index) {
  ASM_CODE_COMMENT(this);
  DCHECK(RootsTable::IsImmortalImmovable(RootIndex::kBuiltinsConstantsTable));
  LoadRoot(destination, RootIndex::kBuiltinsConstantsTable);
  Ld_d(destination,
       FieldMemOperand(destination, FixedArray::kHeaderSize +
                                        constant_index * kPointerSize));
}

void TurboAssembler::LoadRootRelative(Register destination, int32_t offset) {
  Ld_d(destination, MemOperand(kRootRegister, offset));
}

void TurboAssembler::LoadRootRegisterOffset(Register destination,
                                            intptr_t offset) {
  if (offset == 0) {
    Move(destination, kRootRegister);
  } else {
    Add_d(destination, kRootRegister, Operand(offset));
  }
}

void TurboAssembler::Jump(Register target, Condition cond, Register rj,
                          const Operand& rk) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  if (cond == cc_always) {
    jirl(zero_reg, target, 0);
  } else {
    BRANCH_ARGS_CHECK(cond, rj, rk);
    Label skip;
    Branch(&skip, NegateCondition(cond), rj, rk);
    jirl(zero_reg, target, 0);
    bind(&skip);
  }
}

void TurboAssembler::Jump(intptr_t target, RelocInfo::Mode rmode,
                          Condition cond, Register rj, const Operand& rk) {
  Label skip;
  if (cond != cc_always) {
    Branch(&skip, NegateCondition(cond), rj, rk);
  }
  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    li(t7, Operand(target, rmode));
    jirl(zero_reg, t7, 0);
    bind(&skip);
  }
}

void TurboAssembler::Jump(Address target, RelocInfo::Mode rmode, Condition cond,
                          Register rj, const Operand& rk) {
  DCHECK(!RelocInfo::IsCodeTarget(rmode));
  Jump(static_cast<intptr_t>(target), rmode, cond, rj, rk);
}

void TurboAssembler::Jump(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, Register rj, const Operand& rk) {
  DCHECK(RelocInfo::IsCodeTarget(rmode));

  BlockTrampolinePoolScope block_trampoline_pool(this);
  Label skip;
  if (cond != cc_always) {
    BranchShort(&skip, NegateCondition(cond), rj, rk);
  }

  Builtin builtin = Builtin::kNoBuiltinId;
  bool target_is_isolate_independent_builtin =
      isolate()->builtins()->IsBuiltinHandle(code, &builtin) &&
      Builtins::IsIsolateIndependent(builtin);
  if (target_is_isolate_independent_builtin &&
      options().use_pc_relative_calls_and_jumps) {
    int32_t code_target_index = AddCodeTarget(code);
    RecordRelocInfo(RelocInfo::RELATIVE_CODE_TARGET);
    b(code_target_index);
    bind(&skip);
    return;
  } else if (root_array_available_ && options().isolate_independent_code) {
    UNREACHABLE();
    /*int offset = code->builtin_index() * kSystemPointerSize +
                 IsolateData::builtin_entry_table_offset();
    Ld_d(t7, MemOperand(kRootRegister, offset));
    Jump(t7, cc_always, rj, rk);
    bind(&skip);
    return;*/
  } else if (options().inline_offheap_trampolines &&
             target_is_isolate_independent_builtin) {
    // Inline the trampoline.
    RecordCommentForOffHeapTrampoline(builtin);
    li(t7, Operand(BuiltinEntry(builtin), RelocInfo::OFF_HEAP_TARGET));
    Jump(t7, cc_always, rj, rk);
    bind(&skip);
    RecordComment("]");
    return;
  }

  Jump(static_cast<intptr_t>(code.address()), rmode, cc_always, rj, rk);
  bind(&skip);
}

void TurboAssembler::Jump(const ExternalReference& reference) {
  li(t7, reference);
  Jump(t7);
}

// Note: To call gcc-compiled C code on loonarch, you must call through t[0-8].
void TurboAssembler::Call(Register target, Condition cond, Register rj,
                          const Operand& rk) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  if (cond == cc_always) {
    jirl(ra, target, 0);
  } else {
    BRANCH_ARGS_CHECK(cond, rj, rk);
    Label skip;
    Branch(&skip, NegateCondition(cond), rj, rk);
    jirl(ra, target, 0);
    bind(&skip);
  }
  set_pc_for_safepoint();
}

void MacroAssembler::JumpIfIsInRange(Register value, unsigned lower_limit,
                                     unsigned higher_limit,
                                     Label* on_in_range) {
  ASM_CODE_COMMENT(this);
  if (lower_limit != 0) {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    Sub_d(scratch, value, Operand(lower_limit));
    Branch(on_in_range, ls, scratch, Operand(higher_limit - lower_limit));
  } else {
    Branch(on_in_range, ls, value, Operand(higher_limit - lower_limit));
  }
}

void TurboAssembler::Call(Address target, RelocInfo::Mode rmode, Condition cond,
                          Register rj, const Operand& rk) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Label skip;
  if (cond != cc_always) {
    BranchShort(&skip, NegateCondition(cond), rj, rk);
  }
  intptr_t offset_diff = target - pc_offset();
  if (RelocInfo::IsNoInfo(rmode) && is_int28(offset_diff)) {
    bl(offset_diff >> 2);
  } else if (RelocInfo::IsNoInfo(rmode) && is_int38(offset_diff)) {
    pcaddu18i(t7, static_cast<int32_t>(offset_diff) >> 18);
    jirl(ra, t7, (offset_diff & 0x3ffff) >> 2);
  } else {
    li(t7, Operand(static_cast<int64_t>(target), rmode), ADDRESS_LOAD);
    Call(t7, cc_always, rj, rk);
  }
  bind(&skip);
}

void TurboAssembler::Call(Handle<Code> code, RelocInfo::Mode rmode,
                          Condition cond, Register rj, const Operand& rk) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Label skip;
  if (cond != cc_always) {
    BranchShort(&skip, NegateCondition(cond), rj, rk);
  }

  Builtin builtin = Builtin::kNoBuiltinId;
  bool target_is_isolate_independent_builtin =
      isolate()->builtins()->IsBuiltinHandle(code, &builtin) &&
      Builtins::IsIsolateIndependent(builtin);

  if (target_is_isolate_independent_builtin &&
      options().use_pc_relative_calls_and_jumps) {
    int32_t code_target_index = AddCodeTarget(code);
    RecordCommentForOffHeapTrampoline(builtin);
    RecordRelocInfo(RelocInfo::RELATIVE_CODE_TARGET);
    bl(code_target_index);
    set_pc_for_safepoint();
    bind(&skip);
    RecordComment("]");
    return;
  } else if (root_array_available_ && options().isolate_independent_code) {
    UNREACHABLE();
    /*int offset = code->builtin_index() * kSystemPointerSize +
                 IsolateData::builtin_entry_table_offset();
    LoadRootRelative(t7, offset);
    Call(t7, cond, rj, rk);
    bind(&skip);
    return;*/
  } else if (options().inline_offheap_trampolines &&
             target_is_isolate_independent_builtin) {
    // Inline the trampoline.
    RecordCommentForOffHeapTrampoline(builtin);
    li(t7, Operand(BuiltinEntry(builtin), RelocInfo::OFF_HEAP_TARGET));
    Call(t7, cond, rj, rk);
    bind(&skip);
    RecordComment("]");
    return;
  }

  DCHECK(RelocInfo::IsCodeTarget(rmode));
  DCHECK(code->IsExecutable());
  Call(code.address(), rmode, cc_always, rj, rk);
  bind(&skip);
}

void TurboAssembler::LoadEntryFromBuiltinIndex(Register builtin_index) {
  ASM_CODE_COMMENT(this);
  STATIC_ASSERT(kSystemPointerSize == 8);
  STATIC_ASSERT(kSmiTagSize == 1);
  STATIC_ASSERT(kSmiTag == 0);

  // The builtin_index register contains the builtin index as a Smi.
  SmiUntag(builtin_index, builtin_index);
  Alsl_d(builtin_index, builtin_index, kRootRegister, kSystemPointerSizeLog2,
         t7);
  Ld_d(builtin_index,
       MemOperand(builtin_index, IsolateData::builtin_entry_table_offset()));
}

void TurboAssembler::LoadEntryFromBuiltin(Builtin builtin,
                                          Register destination) {
  Ld_d(destination, EntryFromBuiltinAsOperand(builtin));
}
MemOperand TurboAssembler::EntryFromBuiltinAsOperand(Builtin builtin) {
  DCHECK(root_array_available());
  return MemOperand(kRootRegister,
                    IsolateData::BuiltinEntrySlotOffset(builtin));
}

void TurboAssembler::CallBuiltinByIndex(Register builtin_index) {
  ASM_CODE_COMMENT(this);
  LoadEntryFromBuiltinIndex(builtin_index);
  Call(builtin_index);
}
void TurboAssembler::CallBuiltin(Builtin builtin) {
  RecordCommentForOffHeapTrampoline(builtin);
  Call(BuiltinEntry(builtin), RelocInfo::OFF_HEAP_TARGET);
  RecordComment("]");
}

void TurboAssembler::PatchAndJump(Address target) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  pcaddi(scratch, 4);
  Ld_d(t7, MemOperand(scratch, 0));
  jirl(zero_reg, t7, 0);
  nop();
  DCHECK_EQ(reinterpret_cast<uint64_t>(pc_) % 8, 0);
  *reinterpret_cast<uint64_t*>(pc_) = target;  // pc_ should be align.
  pc_ += sizeof(uint64_t);
}

void TurboAssembler::StoreReturnAddressAndCall(Register target) {
  ASM_CODE_COMMENT(this);
  // This generates the final instruction sequence for calls to C functions
  // once an exit frame has been constructed.
  //
  // Note that this assumes the caller code (i.e. the Code object currently
  // being generated) is immovable or that the callee function cannot trigger
  // GC, since the callee function will return to it.

  Assembler::BlockTrampolinePoolScope block_trampoline_pool(this);
  static constexpr int kNumInstructionsToJump = 2;
  Label find_ra;
  // Adjust the value in ra to point to the correct return location, 2nd
  // instruction past the real call into C code (the jirl)), and push it.
  // This is the return address of the exit frame.
  pcaddi(ra, kNumInstructionsToJump + 1);
  bind(&find_ra);

  // This spot was reserved in EnterExitFrame.
  St_d(ra, MemOperand(sp, 0));
  // Stack is still aligned.

  // TODO(LOONG_dev): can be jirl target? a0 -- a7?
  jirl(zero_reg, target, 0);
  // Make sure the stored 'ra' points to this position.
  DCHECK_EQ(kNumInstructionsToJump, InstructionsGeneratedSince(&find_ra));
}

void TurboAssembler::DropArguments(Register count, ArgumentsCountType type,
                                   ArgumentsCountMode mode, Register scratch) {
  switch (type) {
    case kCountIsInteger: {
      Alsl_d(sp, count, sp, kPointerSizeLog2);
      break;
    }
    case kCountIsSmi: {
      STATIC_ASSERT(kSmiTagSize == 1 && kSmiTag == 0);
      DCHECK_NE(scratch, no_reg);
      SmiScale(scratch, count, kPointerSizeLog2);
      Add_d(sp, sp, scratch);
      break;
    }
    case kCountIsBytes: {
      Add_d(sp, sp, count);
      break;
    }
  }
  if (mode == kCountExcludesReceiver) {
    Add_d(sp, sp, kSystemPointerSize);
  }
}

void TurboAssembler::DropArgumentsAndPushNewReceiver(Register argc,
                                                     Register receiver,
                                                     ArgumentsCountType type,
                                                     ArgumentsCountMode mode,
                                                     Register scratch) {
  DCHECK(!AreAliased(argc, receiver));
  if (mode == kCountExcludesReceiver) {
    // Drop arguments without receiver and override old receiver.
    DropArguments(argc, type, kCountIncludesReceiver, scratch);
    St_d(receiver, MemOperand(sp, 0));
  } else {
    DropArguments(argc, type, mode, scratch);
    Push(receiver);
  }
}

void TurboAssembler::Ret(Condition cond, Register rj, const Operand& rk) {
  Jump(ra, cond, rj, rk);
}

void TurboAssembler::Drop(int count, Condition cond, Register reg,
                          const Operand& op) {
  if (count <= 0) {
    return;
  }

  Label skip;

  if (cond != al) {
    Branch(&skip, NegateCondition(cond), reg, op);
  }

  Add_d(sp, sp, Operand(count * kPointerSize));

  if (cond != al) {
    bind(&skip);
  }
}

void MacroAssembler::Swap(Register reg1, Register reg2, Register scratch) {
  if (scratch == no_reg) {
    Xor(reg1, reg1, Operand(reg2));
    Xor(reg2, reg2, Operand(reg1));
    Xor(reg1, reg1, Operand(reg2));
  } else {
    mov(scratch, reg1);
    mov(reg1, reg2);
    mov(reg2, scratch);
  }
}

void TurboAssembler::Call(Label* target) { Branch(target, true); }

void TurboAssembler::Push(Smi smi) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  li(scratch, Operand(smi));
  Push(scratch);
}

void TurboAssembler::Push(Handle<HeapObject> handle) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  li(scratch, Operand(handle));
  Push(scratch);
}

void TurboAssembler::PushArray(Register array, Register size, Register scratch,
                               Register scratch2, PushArrayOrder order) {
  DCHECK(!AreAliased(array, size, scratch, scratch2));
  Label loop, entry;
  if (order == PushArrayOrder::kReverse) {
    mov(scratch, zero_reg);
    jmp(&entry);
    bind(&loop);
    Alsl_d(scratch2, scratch, array, kPointerSizeLog2, t7);
    Ld_d(scratch2, MemOperand(scratch2, 0));
    Push(scratch2);
    Add_d(scratch, scratch, Operand(1));
    bind(&entry);
    Branch(&loop, less, scratch, Operand(size));
  } else {
    mov(scratch, size);
    jmp(&entry);
    bind(&loop);
    Alsl_d(scratch2, scratch, array, kPointerSizeLog2, t7);
    Ld_d(scratch2, MemOperand(scratch2, 0));
    Push(scratch2);
    bind(&entry);
    Add_d(scratch, scratch, Operand(-1));
    Branch(&loop, greater_equal, scratch, Operand(zero_reg));
  }
}

// ---------------------------------------------------------------------------
// Exception handling.

void MacroAssembler::PushStackHandler() {
  // Adjust this code if not the case.
  STATIC_ASSERT(StackHandlerConstants::kSize == 2 * kPointerSize);
  STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0 * kPointerSize);

  Push(Smi::zero());  // Padding.

  // Link the current handler as the next handler.
  li(t2,
     ExternalReference::Create(IsolateAddressId::kHandlerAddress, isolate()));
  Ld_d(t1, MemOperand(t2, 0));
  Push(t1);

  // Set this new handler as the current one.
  St_d(sp, MemOperand(t2, 0));
}

void MacroAssembler::PopStackHandler() {
  STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
  Pop(a1);
  Add_d(sp, sp,
        Operand(
            static_cast<int64_t>(StackHandlerConstants::kSize - kPointerSize)));
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  li(scratch,
     ExternalReference::Create(IsolateAddressId::kHandlerAddress, isolate()));
  St_d(a1, MemOperand(scratch, 0));
}

void TurboAssembler::FPUCanonicalizeNaN(const DoubleRegister dst,
                                        const DoubleRegister src) {
  fsub_d(dst, src, kDoubleRegZero);
}

// -----------------------------------------------------------------------------
// JavaScript invokes.

void MacroAssembler::LoadStackLimit(Register destination, StackLimitKind kind) {
  ASM_CODE_COMMENT(this);
  DCHECK(root_array_available());
  Isolate* isolate = this->isolate();
  ExternalReference limit =
      kind == StackLimitKind::kRealStackLimit
          ? ExternalReference::address_of_real_jslimit(isolate)
          : ExternalReference::address_of_jslimit(isolate);
  DCHECK(TurboAssembler::IsAddressableThroughRootRegister(isolate, limit));

  intptr_t offset =
      TurboAssembler::RootRegisterOffsetForExternalReference(isolate, limit);
  CHECK(is_int32(offset));
  Ld_d(destination, MemOperand(kRootRegister, static_cast<int32_t>(offset)));
}

void MacroAssembler::StackOverflowCheck(Register num_args, Register scratch1,
                                        Register scratch2,
                                        Label* stack_overflow) {
  ASM_CODE_COMMENT(this);
  // Check the stack for overflow. We are not trying to catch
  // interruptions (e.g. debug break and preemption) here, so the "real stack
  // limit" is checked.

  LoadStackLimit(scratch1, StackLimitKind::kRealStackLimit);
  // Make scratch1 the space we have left. The stack might already be overflowed
  // here which will cause scratch1 to become negative.
  sub_d(scratch1, sp, scratch1);
  // Check if the arguments will overflow the stack.
  slli_d(scratch2, num_args, kPointerSizeLog2);
  // Signed comparison.
  Branch(stack_overflow, le, scratch1, Operand(scratch2));
}

void MacroAssembler::InvokePrologue(Register expected_parameter_count,
                                    Register actual_parameter_count,
                                    Label* done, InvokeType type) {
  ASM_CODE_COMMENT(this);
  Label regular_invoke;

  //  a0: actual arguments count
  //  a1: function (passed through to callee)
  //  a2: expected arguments count

  DCHECK_EQ(actual_parameter_count, a0);
  DCHECK_EQ(expected_parameter_count, a2);

  // If the expected parameter count is equal to the adaptor sentinel, no need
  // to push undefined value as arguments.
  if (kDontAdaptArgumentsSentinel != 0) {
    Branch(&regular_invoke, eq, expected_parameter_count,
           Operand(kDontAdaptArgumentsSentinel));
  }

  // If overapplication or if the actual argument count is equal to the
  // formal parameter count, no need to push extra undefined values.
  sub_d(expected_parameter_count, expected_parameter_count,
        actual_parameter_count);
  Branch(&regular_invoke, le, expected_parameter_count, Operand(zero_reg));

  Label stack_overflow;
  StackOverflowCheck(expected_parameter_count, t0, t1, &stack_overflow);
  // Underapplication. Move the arguments already in the stack, including the
  // receiver and the return address.
  {
    Label copy;
    Register src = a6, dest = a7;
    mov(src, sp);
    slli_d(t0, expected_parameter_count, kSystemPointerSizeLog2);
    Sub_d(sp, sp, Operand(t0));
    // Update stack pointer.
    mov(dest, sp);
    mov(t0, actual_parameter_count);
    bind(&copy);
    Ld_d(t1, MemOperand(src, 0));
    St_d(t1, MemOperand(dest, 0));
    Sub_d(t0, t0, Operand(1));
    Add_d(src, src, Operand(kSystemPointerSize));
    Add_d(dest, dest, Operand(kSystemPointerSize));
    if (kJSArgcIncludesReceiver) {
      Branch(&copy, gt, t0, Operand(zero_reg));
    } else {
      Branch(&copy, ge, t0, Operand(zero_reg));
    }
  }

  // Fill remaining expected arguments with undefined values.
  LoadRoot(t0, RootIndex::kUndefinedValue);
  {
    Label loop;
    bind(&loop);
    St_d(t0, MemOperand(a7, 0));
    Sub_d(expected_parameter_count, expected_parameter_count, Operand(1));
    Add_d(a7, a7, Operand(kSystemPointerSize));
    Branch(&loop, gt, expected_parameter_count, Operand(zero_reg));
  }
  b(&regular_invoke);

  bind(&stack_overflow);
  {
    FrameScope frame(
        this, has_frame() ? StackFrame::NO_FRAME_TYPE : StackFrame::INTERNAL);
    CallRuntime(Runtime::kThrowStackOverflow);
    break_(0xCC);
  }

  bind(&regular_invoke);
}

void MacroAssembler::CallDebugOnFunctionCall(Register fun, Register new_target,
                                             Register expected_parameter_count,
                                             Register actual_parameter_count) {
  // Load receiver to pass it later to DebugOnFunctionCall hook.
  LoadReceiver(t0, actual_parameter_count);
  FrameScope frame(
      this, has_frame() ? StackFrame::NO_FRAME_TYPE : StackFrame::INTERNAL);

  SmiTag(expected_parameter_count);
  Push(expected_parameter_count);

  SmiTag(actual_parameter_count);
  Push(actual_parameter_count);

  if (new_target.is_valid()) {
    Push(new_target);
  }
  // TODO(LOONG_dev): MultiPush/Pop
  Push(fun);
  Push(fun);
  Push(t0);
  CallRuntime(Runtime::kDebugOnFunctionCall);
  Pop(fun);
  if (new_target.is_valid()) {
    Pop(new_target);
  }

  Pop(actual_parameter_count);
  SmiUntag(actual_parameter_count);

  Pop(expected_parameter_count);
  SmiUntag(expected_parameter_count);
}

void MacroAssembler::InvokeFunctionCode(Register function, Register new_target,
                                        Register expected_parameter_count,
                                        Register actual_parameter_count,
                                        InvokeType type) {
  // You can't call a function without a valid frame.
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());
  DCHECK_EQ(function, a1);
  DCHECK_IMPLIES(new_target.is_valid(), new_target == a3);

  // On function call, call into the debugger if necessary.
  Label debug_hook, continue_after_hook;
  {
    li(t0, ExternalReference::debug_hook_on_function_call_address(isolate()));
    Ld_b(t0, MemOperand(t0, 0));
    BranchShort(&debug_hook, ne, t0, Operand(zero_reg));
  }
  bind(&continue_after_hook);

  // Clear the new.target register if not given.
  if (!new_target.is_valid()) {
    LoadRoot(a3, RootIndex::kUndefinedValue);
  }

  Label done;
  InvokePrologue(expected_parameter_count, actual_parameter_count, &done, type);
  // We call indirectly through the code field in the function to
  // allow recompilation to take effect without changing any of the
  // call sites.
  Register code = kJavaScriptCallCodeStartRegister;
  Ld_d(code, FieldMemOperand(function, JSFunction::kCodeOffset));
  switch (type) {
    case InvokeType::kCall:
      CallCodeObject(code);
      break;
    case InvokeType::kJump:
      JumpCodeObject(code);
      break;
  }

  Branch(&done);

  // Deferred debug hook.
  bind(&debug_hook);
  CallDebugOnFunctionCall(function, new_target, expected_parameter_count,
                          actual_parameter_count);
  Branch(&continue_after_hook);

  // Continue here if InvokePrologue does handle the invocation due to
  // mismatched parameter counts.
  bind(&done);
}

void MacroAssembler::InvokeFunctionWithNewTarget(
    Register function, Register new_target, Register actual_parameter_count,
    InvokeType type) {
  ASM_CODE_COMMENT(this);
  // You can't call a function without a valid frame.
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());

  // Contract with called JS functions requires that function is passed in a1.
  DCHECK_EQ(function, a1);
  Register expected_parameter_count = a2;
  Register temp_reg = t0;
  Ld_d(temp_reg, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
  Ld_d(cp, FieldMemOperand(a1, JSFunction::kContextOffset));
  // The argument count is stored as uint16_t
  Ld_hu(expected_parameter_count,
        FieldMemOperand(temp_reg,
                        SharedFunctionInfo::kFormalParameterCountOffset));

  InvokeFunctionCode(a1, new_target, expected_parameter_count,
                     actual_parameter_count, type);
}

void MacroAssembler::InvokeFunction(Register function,
                                    Register expected_parameter_count,
                                    Register actual_parameter_count,
                                    InvokeType type) {
  ASM_CODE_COMMENT(this);
  // You can't call a function without a valid frame.
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());

  // Contract with called JS functions requires that function is passed in a1.
  DCHECK_EQ(function, a1);

  // Get the function and setup the context.
  Ld_d(cp, FieldMemOperand(a1, JSFunction::kContextOffset));

  InvokeFunctionCode(a1, no_reg, expected_parameter_count,
                     actual_parameter_count, type);
}

// ---------------------------------------------------------------------------
// Support functions.

void MacroAssembler::GetObjectType(Register object, Register map,
                                   Register type_reg) {
  LoadMap(map, object);
  Ld_hu(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
}

void MacroAssembler::GetInstanceTypeRange(Register map, Register type_reg,
                                          InstanceType lower_limit,
                                          Register range) {
  Ld_hu(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
  Sub_d(range, type_reg, Operand(lower_limit));
}

// -----------------------------------------------------------------------------
// Runtime calls.

void TurboAssembler::AddOverflow_d(Register dst, Register left,
                                   const Operand& right, Register overflow) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Register scratch2 = temps.Acquire();
  Register right_reg = no_reg;
  if (!right.is_reg()) {
    li(scratch, Operand(right));
    right_reg = scratch;
  } else {
    right_reg = right.rm();
  }

  DCHECK(left != scratch2 && right_reg != scratch2 && dst != scratch2 &&
         overflow != scratch2);
  DCHECK(overflow != left && overflow != right_reg);

  if (dst == left || dst == right_reg) {
    add_d(scratch2, left, right_reg);
    xor_(overflow, scratch2, left);
    xor_(scratch, scratch2, right_reg);
    and_(overflow, overflow, scratch);
    mov(dst, scratch2);
  } else {
    add_d(dst, left, right_reg);
    xor_(overflow, dst, left);
    xor_(scratch, dst, right_reg);
    and_(overflow, overflow, scratch);
  }
}

void TurboAssembler::SubOverflow_d(Register dst, Register left,
                                   const Operand& right, Register overflow) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Register scratch2 = temps.Acquire();
  Register right_reg = no_reg;
  if (!right.is_reg()) {
    li(scratch, Operand(right));
    right_reg = scratch;
  } else {
    right_reg = right.rm();
  }

  DCHECK(left != scratch2 && right_reg != scratch2 && dst != scratch2 &&
         overflow != scratch2);
  DCHECK(overflow != left && overflow != right_reg);

  if (dst == left || dst == right_reg) {
    Sub_d(scratch2, left, right_reg);
    xor_(overflow, left, scratch2);
    xor_(scratch, left, right_reg);
    and_(overflow, overflow, scratch);
    mov(dst, scratch2);
  } else {
    sub_d(dst, left, right_reg);
    xor_(overflow, left, dst);
    xor_(scratch, left, right_reg);
    and_(overflow, overflow, scratch);
  }
}

void TurboAssembler::MulOverflow_w(Register dst, Register left,
                                   const Operand& right, Register overflow) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  Register scratch2 = temps.Acquire();
  Register right_reg = no_reg;
  if (!right.is_reg()) {
    li(scratch, Operand(right));
    right_reg = scratch;
  } else {
    right_reg = right.rm();
  }

  DCHECK(left != scratch2 && right_reg != scratch2 && dst != scratch2 &&
         overflow != scratch2);
  DCHECK(overflow != left && overflow != right_reg);

  if (dst == left || dst == right_reg) {
    Mul_w(scratch2, left, right_reg);
    Mulh_w(overflow, left, right_reg);
    mov(dst, scratch2);
  } else {
    Mul_w(dst, left, right_reg);
    Mulh_w(overflow, left, right_reg);
  }

  srai_d(scratch2, dst, 32);
  xor_(overflow, overflow, scratch2);
}

void MacroAssembler::CallRuntime(const Runtime::Function* f, int num_arguments,
                                 SaveFPRegsMode save_doubles) {
  ASM_CODE_COMMENT(this);
  // All parameters are on the stack. v0 has the return value after call.

  // If the expected number of arguments of the runtime function is
  // constant, we check that the actual number of arguments match the
  // expectation.
  CHECK(f->nargs < 0 || f->nargs == num_arguments);

  // TODO(1236192): Most runtime routines don't need the number of
  // arguments passed in because it is constant. At some point we
  // should remove this need and make the runtime routine entry code
  // smarter.
  PrepareCEntryArgs(num_arguments);
  PrepareCEntryFunction(ExternalReference::Create(f));
  Handle<Code> code =
      CodeFactory::CEntry(isolate(), f->result_size, save_doubles);
  Call(code, RelocInfo::CODE_TARGET);
}

void MacroAssembler::TailCallRuntime(Runtime::FunctionId fid) {
  ASM_CODE_COMMENT(this);
  const Runtime::Function* function = Runtime::FunctionForId(fid);
  DCHECK_EQ(1, function->result_size);
  if (function->nargs >= 0) {
    PrepareCEntryArgs(function->nargs);
  }
  JumpToExternalReference(ExternalReference::Create(fid));
}

void MacroAssembler::JumpToExternalReference(const ExternalReference& builtin,
                                             bool builtin_exit_frame) {
  PrepareCEntryFunction(builtin);
  Handle<Code> code = CodeFactory::CEntry(isolate(), 1, SaveFPRegsMode::kIgnore,
                                          ArgvMode::kStack, builtin_exit_frame);
  Jump(code, RelocInfo::CODE_TARGET, al, zero_reg, Operand(zero_reg));
}

void MacroAssembler::JumpToOffHeapInstructionStream(Address entry) {
  li(kOffHeapTrampolineRegister, Operand(entry, RelocInfo::OFF_HEAP_TARGET));
  Jump(kOffHeapTrampolineRegister);
}

void MacroAssembler::LoadWeakValue(Register out, Register in,
                                   Label* target_if_cleared) {
  Branch(target_if_cleared, eq, in, Operand(kClearedWeakHeapObjectLower32));
  And(out, in, Operand(~kWeakHeapObjectMask));
}

void MacroAssembler::EmitIncrementCounter(StatsCounter* counter, int value,
                                          Register scratch1,
                                          Register scratch2) {
  DCHECK_GT(value, 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    ASM_CODE_COMMENT(this);
    // This operation has to be exactly 32-bit wide in case the external
    // reference table redirects the counter to a uint32_t dummy_stats_counter_
    // field.
    li(scratch2, ExternalReference::Create(counter));
    Ld_w(scratch1, MemOperand(scratch2, 0));
    Add_w(scratch1, scratch1, Operand(value));
    St_w(scratch1, MemOperand(scratch2, 0));
  }
}

void MacroAssembler::EmitDecrementCounter(StatsCounter* counter, int value,
                                          Register scratch1,
                                          Register scratch2) {
  DCHECK_GT(value, 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    ASM_CODE_COMMENT(this);
    // This operation has to be exactly 32-bit wide in case the external
    // reference table redirects the counter to a uint32_t dummy_stats_counter_
    // field.
    li(scratch2, ExternalReference::Create(counter));
    Ld_w(scratch1, MemOperand(scratch2, 0));
    Sub_w(scratch1, scratch1, Operand(value));
    St_w(scratch1, MemOperand(scratch2, 0));
  }
}

// -----------------------------------------------------------------------------
// Debugging.

void TurboAssembler::Trap() { stop(); }
void TurboAssembler::DebugBreak() { stop(); }

void TurboAssembler::Assert(Condition cc, AbortReason reason, Register rs,
                            Operand rk) {
  if (FLAG_debug_code) Check(cc, reason, rs, rk);
}

void TurboAssembler::Check(Condition cc, AbortReason reason, Register rj,
                           Operand rk) {
  Label L;
  Branch(&L, cc, rj, rk);
  Abort(reason);
  // Will not return here.
  bind(&L);
}

void TurboAssembler::Abort(AbortReason reason) {
  Label abort_start;
  bind(&abort_start);
  if (FLAG_code_comments) {
    const char* msg = GetAbortReason(reason);
    RecordComment("Abort message: ");
    RecordComment(msg);
  }

  // Avoid emitting call to builtin if requested.
  if (trap_on_abort()) {
    stop();
    return;
  }

  if (should_abort_hard()) {
    // We don't care if we constructed a frame. Just pretend we did.
    FrameScope assume_frame(this, StackFrame::NO_FRAME_TYPE);
    PrepareCallCFunction(0, a0);
    li(a0, Operand(static_cast<int>(reason)));
    CallCFunction(ExternalReference::abort_with_reason(), 1);
    return;
  }

  Move(a0, Smi::FromInt(static_cast<int>(reason)));

  // Disable stub call restrictions to always allow calls to abort.
  if (!has_frame()) {
    // We don't actually want to generate a pile of code for this, so just
    // claim there is a stack frame, without generating one.
    FrameScope scope(this, StackFrame::NO_FRAME_TYPE);
    Call(BUILTIN_CODE(isolate(), Abort), RelocInfo::CODE_TARGET);
  } else {
    Call(BUILTIN_CODE(isolate(), Abort), RelocInfo::CODE_TARGET);
  }
  // Will not return here.
  if (is_trampoline_pool_blocked()) {
    // If the calling code cares about the exact number of
    // instructions generated, we insert padding here to keep the size
    // of the Abort macro constant.
    // Currently in debug mode with debug_code enabled the number of
    // generated instructions is 10, so we use this as a maximum value.
    static const int kExpectedAbortInstructions = 10;
    int abort_instructions = InstructionsGeneratedSince(&abort_start);
    DCHECK_LE(abort_instructions, kExpectedAbortInstructions);
    while (abort_instructions++ < kExpectedAbortInstructions) {
      nop();
    }
  }
}

void TurboAssembler::LoadMap(Register destination, Register object) {
  Ld_d(destination, FieldMemOperand(object, HeapObject::kMapOffset));
}

void MacroAssembler::LoadNativeContextSlot(Register dst, int index) {
  LoadMap(dst, cp);
  Ld_d(dst, FieldMemOperand(
                dst, Map::kConstructorOrBackPointerOrNativeContextOffset));
  Ld_d(dst, MemOperand(dst, Context::SlotOffset(index)));
}

void TurboAssembler::StubPrologue(StackFrame::Type type) {
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  li(scratch, Operand(StackFrame::TypeToMarker(type)));
  PushCommonFrame(scratch);
}

void TurboAssembler::Prologue() { PushStandardFrame(a1); }

void TurboAssembler::EnterFrame(StackFrame::Type type) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Push(ra, fp);
  Move(fp, sp);
  if (!StackFrame::IsJavaScript(type)) {
    li(kScratchReg, Operand(StackFrame::TypeToMarker(type)));
    Push(kScratchReg);
  }
#if V8_ENABLE_WEBASSEMBLY
  if (type == StackFrame::WASM) Push(kWasmInstanceRegister);
#endif  // V8_ENABLE_WEBASSEMBLY
}

void TurboAssembler::LeaveFrame(StackFrame::Type type) {
  ASM_CODE_COMMENT(this);
  addi_d(sp, fp, 2 * kPointerSize);
  Ld_d(ra, MemOperand(fp, 1 * kPointerSize));
  Ld_d(fp, MemOperand(fp, 0 * kPointerSize));
}

void MacroAssembler::EnterExitFrame(bool save_doubles, int stack_space,
                                    StackFrame::Type frame_type) {
  ASM_CODE_COMMENT(this);
  DCHECK(frame_type == StackFrame::EXIT ||
         frame_type == StackFrame::BUILTIN_EXIT);

  // Set up the frame structure on the stack.
  STATIC_ASSERT(2 * kPointerSize == ExitFrameConstants::kCallerSPDisplacement);
  STATIC_ASSERT(1 * kPointerSize == ExitFrameConstants::kCallerPCOffset);
  STATIC_ASSERT(0 * kPointerSize == ExitFrameConstants::kCallerFPOffset);

  // This is how the stack will look:
  // fp + 2 (==kCallerSPDisplacement) - old stack's end
  // [fp + 1 (==kCallerPCOffset)] - saved old ra
  // [fp + 0 (==kCallerFPOffset)] - saved old fp
  // [fp - 1 StackFrame::EXIT Smi
  // [fp - 2 (==kSPOffset)] - sp of the called function
  // fp - (2 + stack_space + alignment) == sp == [fp - kSPOffset] - top of the
  //   new stack (will contain saved ra)

  // Save registers and reserve room for saved entry sp.
  addi_d(sp, sp, -2 * kPointerSize - ExitFrameConstants::kFixedFrameSizeFromFp);
  St_d(ra, MemOperand(sp, 3 * kPointerSize));
  St_d(fp, MemOperand(sp, 2 * kPointerSize));
  {
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    li(scratch, Operand(StackFrame::TypeToMarker(frame_type)));
    St_d(scratch, MemOperand(sp, 1 * kPointerSize));
  }
  // Set up new frame pointer.
  addi_d(fp, sp, ExitFrameConstants::kFixedFrameSizeFromFp);

  if (FLAG_debug_code) {
    St_d(zero_reg, MemOperand(fp, ExitFrameConstants::kSPOffset));
  }

  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    // Save the frame pointer and the context in top.
    li(t8, ExternalReference::Create(IsolateAddressId::kCEntryFPAddress,
                                     isolate()));
    St_d(fp, MemOperand(t8, 0));
    li(t8,
       ExternalReference::Create(IsolateAddressId::kContextAddress, isolate()));
    St_d(cp, MemOperand(t8, 0));
  }

  const int frame_alignment = MacroAssembler::ActivationFrameAlignment();
  if (save_doubles) {
    // The stack is already aligned to 0 modulo 8 for stores with sdc1.
    int kNumOfSavedRegisters = FPURegister::kNumRegisters / 2;
    int space = kNumOfSavedRegisters * kDoubleSize;
    Sub_d(sp, sp, Operand(space));
    // Remember: we only need to save every 2nd double FPU value.
    for (int i = 0; i < kNumOfSavedRegisters; i++) {
      FPURegister reg = FPURegister::from_code(2 * i);
      Fst_d(reg, MemOperand(sp, i * kDoubleSize));
    }
  }

  // Reserve place for the return address, stack space and an optional slot
  // (used by DirectCEntry to hold the return value if a struct is
  // returned) and align the frame preparing for calling the runtime function.
  DCHECK_GE(stack_space, 0);
  Sub_d(sp, sp, Operand((stack_space + 2) * kPointerSize));
  if (frame_alignment > 0) {
    DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
    And(sp, sp, Operand(-frame_alignment));  // Align stack.
  }

  // Set the exit frame sp value to point just before the return address
  // location.
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  addi_d(scratch, sp, kPointerSize);
  St_d(scratch, MemOperand(fp, ExitFrameConstants::kSPOffset));
}

void MacroAssembler::LeaveExitFrame(bool save_doubles, Register argument_count,
                                    bool do_return,
                                    bool argument_count_is_length) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  // Optionally restore all double registers.
  if (save_doubles) {
    // Remember: we only need to restore every 2nd double FPU value.
    int kNumOfSavedRegisters = FPURegister::kNumRegisters / 2;
    Sub_d(t8, fp,
          Operand(ExitFrameConstants::kFixedFrameSizeFromFp +
                  kNumOfSavedRegisters * kDoubleSize));
    for (int i = 0; i < kNumOfSavedRegisters; i++) {
      FPURegister reg = FPURegister::from_code(2 * i);
      Fld_d(reg, MemOperand(t8, i * kDoubleSize));
    }
  }

  // Clear top frame.
  li(t8,
     ExternalReference::Create(IsolateAddressId::kCEntryFPAddress, isolate()));
  St_d(zero_reg, MemOperand(t8, 0));

  // Restore current context from top and clear it in debug mode.
  li(t8,
     ExternalReference::Create(IsolateAddressId::kContextAddress, isolate()));
  Ld_d(cp, MemOperand(t8, 0));

  if (FLAG_debug_code) {
    UseScratchRegisterScope temp(this);
    Register scratch = temp.Acquire();
    li(scratch, Operand(Context::kInvalidContext));
    St_d(scratch, MemOperand(t8, 0));
  }

  // Pop the arguments, restore registers, and return.
  mov(sp, fp);  // Respect ABI stack constraint.
  Ld_d(fp, MemOperand(sp, ExitFrameConstants::kCallerFPOffset));
  Ld_d(ra, MemOperand(sp, ExitFrameConstants::kCallerPCOffset));

  if (argument_count.is_valid()) {
    if (argument_count_is_length) {
      add_d(sp, sp, argument_count);
    } else {
      Alsl_d(sp, argument_count, sp, kPointerSizeLog2, t8);
    }
  }

  addi_d(sp, sp, 2 * kPointerSize);
  if (do_return) {
    Ret();
  }
}

int TurboAssembler::ActivationFrameAlignment() {
#if V8_HOST_ARCH_LOONG64
  // Running on the real platform. Use the alignment as mandated by the local
  // environment.
  // Note: This will break if we ever start generating snapshots on one LOONG64
  // platform for another LOONG64 platform with a different alignment.
  return base::OS::ActivationFrameAlignment();
#else   // V8_HOST_ARCH_LOONG64
  // If we are using the simulator then we should always align to the expected
  // alignment. As the simulator is used to generate snapshots we do not know
  // if the target platform will need alignment, so this is controlled from a
  // flag.
  return FLAG_sim_stack_alignment;
#endif  // V8_HOST_ARCH_LOONG64
}

void MacroAssembler::AssertStackIsAligned() {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    const int frame_alignment = ActivationFrameAlignment();
    const int frame_alignment_mask = frame_alignment - 1;

    if (frame_alignment > kPointerSize) {
      Label alignment_as_expected;
      DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
      {
        UseScratchRegisterScope temps(this);
        Register scratch = temps.Acquire();
        andi(scratch, sp, frame_alignment_mask);
        Branch(&alignment_as_expected, eq, scratch, Operand(zero_reg));
      }
      // Don't use Check here, as it will call Runtime_Abort re-entering here.
      stop();
      bind(&alignment_as_expected);
    }
  }
}

void TurboAssembler::SmiUntag(Register dst, const MemOperand& src) {
  if (SmiValuesAre32Bits()) {
    Ld_w(dst, MemOperand(src.base(), SmiWordOffset(src.offset())));
  } else {
    DCHECK(SmiValuesAre31Bits());
    Ld_w(dst, src);
    SmiUntag(dst);
  }
}

void TurboAssembler::JumpIfSmi(Register value, Label* smi_label) {
  DCHECK_EQ(0, kSmiTag);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  andi(scratch, value, kSmiTagMask);
  Branch(smi_label, eq, scratch, Operand(zero_reg));
}

void MacroAssembler::JumpIfNotSmi(Register value, Label* not_smi_label) {
  DCHECK_EQ(0, kSmiTag);
  UseScratchRegisterScope temps(this);
  Register scratch = temps.Acquire();
  andi(scratch, value, kSmiTagMask);
  Branch(not_smi_label, ne, scratch, Operand(zero_reg));
}

void TurboAssembler::AssertNotSmi(Register object) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    STATIC_ASSERT(kSmiTag == 0);
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    andi(scratch, object, kSmiTagMask);
    Check(ne, AbortReason::kOperandIsASmi, scratch, Operand(zero_reg));
  }
}

void TurboAssembler::AssertSmi(Register object) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    STATIC_ASSERT(kSmiTag == 0);
    UseScratchRegisterScope temps(this);
    Register scratch = temps.Acquire();
    andi(scratch, object, kSmiTagMask);
    Check(eq, AbortReason::kOperandIsASmi, scratch, Operand(zero_reg));
  }
}

void MacroAssembler::AssertConstructor(Register object) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    BlockTrampolinePoolScope block_trampoline_pool(this);
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, AbortReason::kOperandIsASmiAndNotAConstructor, t8,
          Operand(zero_reg));

    LoadMap(t8, object);
    Ld_bu(t8, FieldMemOperand(t8, Map::kBitFieldOffset));
    And(t8, t8, Operand(Map::Bits1::IsConstructorBit::kMask));
    Check(ne, AbortReason::kOperandIsNotAConstructor, t8, Operand(zero_reg));
  }
}

void MacroAssembler::AssertFunction(Register object) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    BlockTrampolinePoolScope block_trampoline_pool(this);
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, AbortReason::kOperandIsASmiAndNotAFunction, t8,
          Operand(zero_reg));
    Push(object);
    LoadMap(object, object);
    GetInstanceTypeRange(object, object, FIRST_JS_FUNCTION_TYPE, t8);
    Check(ls, AbortReason::kOperandIsNotAFunction, t8,
          Operand(LAST_JS_FUNCTION_TYPE - FIRST_JS_FUNCTION_TYPE));
    Pop(object);
  }
}

void MacroAssembler::AssertCallableFunction(Register object) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    BlockTrampolinePoolScope block_trampoline_pool(this);
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, AbortReason::kOperandIsASmiAndNotAFunction, t8,
          Operand(zero_reg));
    Push(object);
    LoadMap(object, object);
    GetInstanceTypeRange(object, object, FIRST_CALLABLE_JS_FUNCTION_TYPE, t8);
    Check(ls, AbortReason::kOperandIsNotACallableFunction, t8,
          Operand(LAST_CALLABLE_JS_FUNCTION_TYPE -
                  FIRST_CALLABLE_JS_FUNCTION_TYPE));
    Pop(object);
  }
}

void MacroAssembler::AssertBoundFunction(Register object) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    BlockTrampolinePoolScope block_trampoline_pool(this);
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, AbortReason::kOperandIsASmiAndNotABoundFunction, t8,
          Operand(zero_reg));
    GetObjectType(object, t8, t8);
    Check(eq, AbortReason::kOperandIsNotABoundFunction, t8,
          Operand(JS_BOUND_FUNCTION_TYPE));
  }
}

void MacroAssembler::AssertGeneratorObject(Register object) {
  if (!FLAG_debug_code) return;
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  STATIC_ASSERT(kSmiTag == 0);
  SmiTst(object, t8);
  Check(ne, AbortReason::kOperandIsASmiAndNotAGeneratorObject, t8,
        Operand(zero_reg));

  GetObjectType(object, t8, t8);

  Label done;

  // Check if JSGeneratorObject
  Branch(&done, eq, t8, Operand(JS_GENERATOR_OBJECT_TYPE));

  // Check if JSAsyncFunctionObject (See MacroAssembler::CompareInstanceType)
  Branch(&done, eq, t8, Operand(JS_ASYNC_FUNCTION_OBJECT_TYPE));

  // Check if JSAsyncGeneratorObject
  Branch(&done, eq, t8, Operand(JS_ASYNC_GENERATOR_OBJECT_TYPE));

  Abort(AbortReason::kOperandIsNotAGeneratorObject);

  bind(&done);
}

void MacroAssembler::AssertUndefinedOrAllocationSite(Register object,
                                                     Register scratch) {
  if (FLAG_debug_code) {
    ASM_CODE_COMMENT(this);
    Label done_checking;
    AssertNotSmi(object);
    LoadRoot(scratch, RootIndex::kUndefinedValue);
    Branch(&done_checking, eq, object, Operand(scratch));
    GetObjectType(object, scratch, scratch);
    Assert(eq, AbortReason::kExpectedUndefinedOrCell, scratch,
           Operand(ALLOCATION_SITE_TYPE));
    bind(&done_checking);
  }
}

void TurboAssembler::Float32Max(FPURegister dst, FPURegister src1,
                                FPURegister src2, Label* out_of_line) {
  ASM_CODE_COMMENT(this);
  if (src1 == src2) {
    Move_s(dst, src1);
    return;
  }

  // Check if one of operands is NaN.
  CompareIsNanF32(src1, src2);
  BranchTrueF(out_of_line);

  fmax_s(dst, src1, src2);
}

void TurboAssembler::Float32MaxOutOfLine(FPURegister dst, FPURegister src1,
                                         FPURegister src2) {
  fadd_s(dst, src1, src2);
}

void TurboAssembler::Float32Min(FPURegister dst, FPURegister src1,
                                FPURegister src2, Label* out_of_line) {
  ASM_CODE_COMMENT(this);
  if (src1 == src2) {
    Move_s(dst, src1);
    return;
  }

  // Check if one of operands is NaN.
  CompareIsNanF32(src1, src2);
  BranchTrueF(out_of_line);

  fmin_s(dst, src1, src2);
}

void TurboAssembler::Float32MinOutOfLine(FPURegister dst, FPURegister src1,
                                         FPURegister src2) {
  fadd_s(dst, src1, src2);
}

void TurboAssembler::Float64Max(FPURegister dst, FPURegister src1,
                                FPURegister src2, Label* out_of_line) {
  ASM_CODE_COMMENT(this);
  if (src1 == src2) {
    Move_d(dst, src1);
    return;
  }

  // Check if one of operands is NaN.
  CompareIsNanF64(src1, src2);
  BranchTrueF(out_of_line);

  fmax_d(dst, src1, src2);
}

void TurboAssembler::Float64MaxOutOfLine(FPURegister dst, FPURegister src1,
                                         FPURegister src2) {
  fadd_d(dst, src1, src2);
}

void TurboAssembler::Float64Min(FPURegister dst, FPURegister src1,
                                FPURegister src2, Label* out_of_line) {
  ASM_CODE_COMMENT(this);
  if (src1 == src2) {
    Move_d(dst, src1);
    return;
  }

  // Check if one of operands is NaN.
  CompareIsNanF64(src1, src2);
  BranchTrueF(out_of_line);

  fmin_d(dst, src1, src2);
}

void TurboAssembler::Float64MinOutOfLine(FPURegister dst, FPURegister src1,
                                         FPURegister src2) {
  fadd_d(dst, src1, src2);
}

static const int kRegisterPassedArguments = 8;

int TurboAssembler::CalculateStackPassedWords(int num_reg_arguments,
                                              int num_double_arguments) {
  int stack_passed_words = 0;
  num_reg_arguments += 2 * num_double_arguments;

  // Up to eight simple arguments are passed in registers a0..a7.
  if (num_reg_arguments > kRegisterPassedArguments) {
    stack_passed_words += num_reg_arguments - kRegisterPassedArguments;
  }
  return stack_passed_words;
}

void TurboAssembler::PrepareCallCFunction(int num_reg_arguments,
                                          int num_double_arguments,
                                          Register scratch) {
  ASM_CODE_COMMENT(this);
  int frame_alignment = ActivationFrameAlignment();

  // Up to eight simple arguments in a0..a3, a4..a7, No argument slots.
  // Remaining arguments are pushed on the stack.
  int stack_passed_arguments =
      CalculateStackPassedWords(num_reg_arguments, num_double_arguments);
  if (frame_alignment > kPointerSize) {
    // Make stack end at alignment and make room for num_arguments - 4 words
    // and the original value of sp.
    mov(scratch, sp);
    Sub_d(sp, sp, Operand((stack_passed_arguments + 1) * kPointerSize));
    DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
    bstrins_d(sp, zero_reg, std::log2(frame_alignment) - 1, 0);
    St_d(scratch, MemOperand(sp, stack_passed_arguments * kPointerSize));
  } else {
    Sub_d(sp, sp, Operand(stack_passed_arguments * kPointerSize));
  }
}

void TurboAssembler::PrepareCallCFunction(int num_reg_arguments,
                                          Register scratch) {
  PrepareCallCFunction(num_reg_arguments, 0, scratch);
}

void TurboAssembler::CallCFunction(ExternalReference function,
                                   int num_reg_arguments,
                                   int num_double_arguments) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  li(t7, function);
  CallCFunctionHelper(t7, num_reg_arguments, num_double_arguments);
}

void TurboAssembler::CallCFunction(Register function, int num_reg_arguments,
                                   int num_double_arguments) {
  ASM_CODE_COMMENT(this);
  CallCFunctionHelper(function, num_reg_arguments, num_double_arguments);
}

void TurboAssembler::CallCFunction(ExternalReference function,
                                   int num_arguments) {
  CallCFunction(function, num_arguments, 0);
}

void TurboAssembler::CallCFunction(Register function, int num_arguments) {
  CallCFunction(function, num_arguments, 0);
}

void TurboAssembler::CallCFunctionHelper(Register function,
                                         int num_reg_arguments,
                                         int num_double_arguments) {
  DCHECK_LE(num_reg_arguments + num_double_arguments, kMaxCParameters);
  DCHECK(has_frame());
  // Make sure that the stack is aligned before calling a C function unless
  // running in the simulator. The simulator has its own alignment check which
  // provides more information.

#if V8_HOST_ARCH_LOONG64
  if (FLAG_debug_code) {
    int frame_alignment = base::OS::ActivationFrameAlignment();
    int frame_alignment_mask = frame_alignment - 1;
    if (frame_alignment > kPointerSize) {
      DCHECK(base::bits::IsPowerOfTwo(frame_alignment));
      Label alignment_as_expected;
      {
        Register scratch = t8;
        And(scratch, sp, Operand(frame_alignment_mask));
        Branch(&alignment_as_expected, eq, scratch, Operand(zero_reg));
      }
      // Don't use Check here, as it will call Runtime_Abort possibly
      // re-entering here.
      stop();
      bind(&alignment_as_expected);
    }
  }
#endif  // V8_HOST_ARCH_LOONG64

  // Just call directly. The function called cannot cause a GC, or
  // allow preemption, so the return address in the link register
  // stays correct.
  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    if (function != t7) {
      mov(t7, function);
      function = t7;
    }

    // Save the frame pointer and PC so that the stack layout remains iterable,
    // even without an ExitFrame which normally exists between JS and C frames.
    // 't' registers are caller-saved so this is safe as a scratch register.
    Register pc_scratch = t1;
    Register scratch = t2;
    DCHECK(!AreAliased(pc_scratch, scratch, function));

    pcaddi(pc_scratch, 1);

    // See x64 code for reasoning about how to address the isolate data fields.
    if (root_array_available()) {
      St_d(pc_scratch, MemOperand(kRootRegister,
                                  IsolateData::fast_c_call_caller_pc_offset()));
      St_d(fp, MemOperand(kRootRegister,
                          IsolateData::fast_c_call_caller_fp_offset()));
    } else {
      DCHECK_NOT_NULL(isolate());
      li(scratch, ExternalReference::fast_c_call_caller_pc_address(isolate()));
      St_d(pc_scratch, MemOperand(scratch, 0));
      li(scratch, ExternalReference::fast_c_call_caller_fp_address(isolate()));
      St_d(fp, MemOperand(scratch, 0));
    }

    Call(function);

    // We don't unset the PC; the FP is the source of truth.
    if (root_array_available()) {
      St_d(zero_reg, MemOperand(kRootRegister,
                                IsolateData::fast_c_call_caller_fp_offset()));
    } else {
      DCHECK_NOT_NULL(isolate());
      li(scratch, ExternalReference::fast_c_call_caller_fp_address(isolate()));
      St_d(zero_reg, MemOperand(scratch, 0));
    }

    int stack_passed_arguments =
        CalculateStackPassedWords(num_reg_arguments, num_double_arguments);

    if (base::OS::ActivationFrameAlignment() > kPointerSize) {
      Ld_d(sp, MemOperand(sp, stack_passed_arguments * kPointerSize));
    } else {
      Add_d(sp, sp, Operand(stack_passed_arguments * kPointerSize));
    }

    set_pc_for_safepoint();
  }
}

#undef BRANCH_ARGS_CHECK

void TurboAssembler::CheckPageFlag(const Register& object, int mask,
                                   Condition cc, Label* condition_met) {
  ASM_CODE_COMMENT(this);
  UseScratchRegisterScope temps(this);
  temps.Include(t8);
  Register scratch = temps.Acquire();
  And(scratch, object, Operand(~kPageAlignmentMask));
  Ld_d(scratch, MemOperand(scratch, BasicMemoryChunk::kFlagsOffset));
  And(scratch, scratch, Operand(mask));
  Branch(condition_met, cc, scratch, Operand(zero_reg));
}

Register GetRegisterThatIsNotOneOf(Register reg1, Register reg2, Register reg3,
                                   Register reg4, Register reg5,
                                   Register reg6) {
  RegList regs = 0;
  if (reg1.is_valid()) regs |= reg1.bit();
  if (reg2.is_valid()) regs |= reg2.bit();
  if (reg3.is_valid()) regs |= reg3.bit();
  if (reg4.is_valid()) regs |= reg4.bit();
  if (reg5.is_valid()) regs |= reg5.bit();
  if (reg6.is_valid()) regs |= reg6.bit();

  const RegisterConfiguration* config = RegisterConfiguration::Default();
  for (int i = 0; i < config->num_allocatable_general_registers(); ++i) {
    int code = config->GetAllocatableGeneralCode(i);
    Register candidate = Register::from_code(code);
    if (regs & candidate.bit()) continue;
    return candidate;
  }
  UNREACHABLE();
}

void TurboAssembler::ComputeCodeStartAddress(Register dst) {
  // TODO(LOONG_dev): range check, add Pcadd macro function?
  pcaddi(dst, -pc_offset() >> 2);
}

void TurboAssembler::CallForDeoptimization(Builtin target, int, Label* exit,
                                           DeoptimizeKind kind, Label* ret,
                                           Label*) {
  ASM_CODE_COMMENT(this);
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Ld_d(t7,
       MemOperand(kRootRegister, IsolateData::BuiltinEntrySlotOffset(target)));
  Call(t7);
  DCHECK_EQ(SizeOfCodeGeneratedSince(exit),
            (kind == DeoptimizeKind::kLazy)
                ? Deoptimizer::kLazyDeoptExitSize
                : Deoptimizer::kNonLazyDeoptExitSize);

  if (kind == DeoptimizeKind::kEagerWithResume) {
    Branch(ret);
    DCHECK_EQ(SizeOfCodeGeneratedSince(exit),
              Deoptimizer::kEagerWithResumeBeforeArgsSize);
  }
}

void TurboAssembler::LoadCodeObjectEntry(Register destination,
                                         Register code_object) {
  ASM_CODE_COMMENT(this);
  // Code objects are called differently depending on whether we are generating
  // builtin code (which will later be embedded into the binary) or compiling
  // user JS code at runtime.
  // * Builtin code runs in --jitless mode and thus must not call into on-heap
  //   Code targets. Instead, we dispatch through the builtins entry table.
  // * Codegen at runtime does not have this restriction and we can use the
  //   shorter, branchless instruction sequence. The assumption here is that
  //   targets are usually generated code and not builtin Code objects.
  if (options().isolate_independent_code) {
    DCHECK(root_array_available());
    Label if_code_is_off_heap, out;
    Register scratch = t8;

    DCHECK(!AreAliased(destination, scratch));
    DCHECK(!AreAliased(code_object, scratch));

    // Check whether the Code object is an off-heap trampoline. If so, call its
    // (off-heap) entry point directly without going through the (on-heap)
    // trampoline.  Otherwise, just call the Code object as always.
    Ld_w(scratch, FieldMemOperand(code_object, Code::kFlagsOffset));
    And(scratch, scratch, Operand(Code::IsOffHeapTrampoline::kMask));
    BranchShort(&if_code_is_off_heap, ne, scratch, Operand(zero_reg));
    // Not an off-heap trampoline object, the entry point is at
    // Code::raw_instruction_start().
    Add_d(destination, code_object, Code::kHeaderSize - kHeapObjectTag);
    Branch(&out);

    // An off-heap trampoline, the entry point is loaded from the builtin entry
    // table.
    bind(&if_code_is_off_heap);
    Ld_w(scratch, FieldMemOperand(code_object, Code::kBuiltinIndexOffset));
    // TODO(liuyu): don't use scratch_reg in Alsl_d;
    Alsl_d(destination, scratch, kRootRegister, kSystemPointerSizeLog2,
           zero_reg);
    Ld_d(destination,
         MemOperand(destination, IsolateData::builtin_entry_table_offset()));

    bind(&out);
  } else {
    Add_d(destination, code_object, Code::kHeaderSize - kHeapObjectTag);
  }
}

void TurboAssembler::CallCodeObject(Register code_object) {
  ASM_CODE_COMMENT(this);
  LoadCodeObjectEntry(code_object, code_object);
  Call(code_object);
}

void TurboAssembler::JumpCodeObject(Register code_object, JumpMode jump_mode) {
  ASM_CODE_COMMENT(this);
  DCHECK_EQ(JumpMode::kJump, jump_mode);
  LoadCodeObjectEntry(code_object, code_object);
  Jump(code_object);
}

}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_LOONG64
