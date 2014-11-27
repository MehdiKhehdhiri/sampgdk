/* Copyright (C) 2012-2014 Zeex
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <sampgdk/bool.h>
#include <sampgdk/platform.h>

#if SAMPGDK_WINDOWS
  #include <windows.h>
  typedef __int32 _sampgdk_hook_int32_t;
#else
  #include <stdint.h>
  #include <unistd.h>
  #include <sys/mman.h>
  typedef int32_t _sampgdk_hook_int32_t;
#endif

#include "log.h"
#include "hook.h"

#define _SAMPGDK_HOOK_JMP_SIZE 5
#define _SAMPGDK_HOOK_MAX_INSN_LEN 15
#define _SAMPGDK_HOOK_TRAMPOLINE_SIZE \
  (_SAMPGDK_HOOK_JMP_SIZE + _SAMPGDK_HOOK_MAX_INSN_LEN - 1)

#pragma pack(push, 1)

struct _sampgdk_hook_jmp {
  unsigned char         opcode;
  _sampgdk_hook_int32_t offset;
};

#pragma pack(pop)

struct _sampgdk_hook {
  unsigned char trampoline[_SAMPGDK_HOOK_TRAMPOLINE_SIZE];
};

#if SAMPGDK_WINDOWS

static void *_sampgdk_hook_unprotect(void *address, size_t size) {
  DWORD old;

  if (VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old) == 0) {
    return NULL;
  }

  return address;
}

#else /* SAMPGDK_WINDOWS */

static void *_sampgdk_hook_unprotect(void *address, size_t size) {
  long pagesize;

  pagesize = sysconf(_SC_PAGESIZE);
  address = (void *)((long)address & ~(pagesize - 1));

  if (mprotect(address, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return NULL;
  }

  return address;
}

#endif /* !SAMPGDK_WINDOWS */

static size_t _sampgdk_hook_get_insn_len(unsigned char *code) {
  enum flags {
    MODRM      = 1,      /* ModRM byte is present */
    REG_OPCODE = 1 << 1, /* ModRM.reg is part of opcode */
    IMM8       = 1 << 2, /* 8-bit immediate */
    IMM16      = 1 << 3, /* 16-bit immediate */
    IMM32      = 1 << 4, /* 16/32-bit immediate (size depends on prefix) */
    PLUS_R     = 1 << 5, /* register operand encoded into opcode */
  };

  static int prefixes[] = {
    0xF0, 0xF2, 0xF3,
    0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65,
    0x66,
    0x67
  };

  struct opcode_info {
    int opcode;
    int reg_opcode;
    int flags;
  };

  static struct opcode_info opcodes[] = {
    /* CALL rel32       */ {0xE8, 0, IMM32},
    /* CALL r/m32       */ {0xFF, 2, MODRM | REG_OPCODE},
    /* JMP rel32        */ {0xE9, 0, IMM32},
    /* JMP r/m32        */ {0xFF, 4, MODRM | REG_OPCODE},
    /* LEA r16,m        */ {0x8D, 0, MODRM},
    /* MOV r/m8,r8      */ {0x88, 0, MODRM},
    /* MOV r/m32,r32    */ {0x89, 0, MODRM},
    /* MOV r8,r/m8      */ {0x8A, 0, MODRM},
    /* MOV r32,r/m32    */ {0x8B, 0, MODRM},
    /* MOV r/m16,Sreg   */ {0x8C, 0, MODRM},
    /* MOV Sreg,r/m16   */ {0x8E, 0, MODRM},
    /* MOV AL,moffs8    */ {0xA0, 0, IMM8},
    /* MOV EAX,moffs32  */ {0xA1, 0, IMM32},
    /* MOV moffs8,AL    */ {0xA2, 0, IMM8},
    /* MOV moffs32,EAX  */ {0xA3, 0, IMM32},
    /* MOV r8, imm8     */ {0xB0, 0, PLUS_R | IMM8},
    /* MOV r32, imm32   */ {0xB8, 0, PLUS_R | IMM32},
    /* MOV r/m8, imm8   */ {0xC6, 0, MODRM | REG_OPCODE | IMM8},
    /* MOV r/m32, imm32 */ {0xC7, 0, MODRM | REG_OPCODE | IMM32},
    /* POP r/m32        */ {0x8F, 0, MODRM | REG_OPCODE},
    /* POP r32          */ {0x58, 0, PLUS_R},
    /* PUSH r/m32       */ {0xFF, 6, MODRM | REG_OPCODE},
    /* PUSH r32         */ {0x50, 0, PLUS_R},
    /* PUSH imm8        */ {0x6A, 0, IMM8},
    /* PUSH imm32       */ {0x68, 0, IMM32},
    /* RET              */ {0xC3, 0, 0},
    /* RET imm16        */ {0xC2, 0, IMM16},
    /* SUB r/m32, imm8  */ {0x83, 5, MODRM | REG_OPCODE | IMM8},
    /* SUB r/m32, r32   */ {0x29, 0, MODRM},
    /* SUB r32, r/m32   */ {0x2B, 0, MODRM}
  };

  int i;
  int len = 0;
  int opcode = 0;

  for (i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++) {
    if (code[len] == prefixes[i]) {
      len++;
    }
  }

  for (i = 0; i < sizeof(opcodes) / sizeof(*opcodes); i++) {
    bool found = false;

    if (code[len] == opcodes[i].opcode) {
      found = !(opcodes[i].flags & REG_OPCODE)
            || ((code[len + 1] >> 3) & 7) == opcodes[i].reg_opcode;
    }
    if ((opcodes[i].flags & PLUS_R)
        && (code[len] & 0xF8) == opcodes[i].opcode) {
      found = true;
    }

    if (found) {
      opcode = code[len++];
      break;
    }
  }

  if (opcode == 0) {
    return 0;
  }

  if (opcodes[i].flags & MODRM) {
    int modrm = code[len++];
    int mod = modrm >> 6;
    int rm = modrm & 7;

    if (mod != 3 && rm == 4) {
      len++; /* for SIB */
    }

    if (mod == 1) len += 1; /* [reg + disp8] */
    if (mod == 2) len += 4; /* [reg + disp32] */

    if (mod == 0 && rm == 5) {
      len += 4; /* [disp32] */
    }
  }

  if (opcodes[i].flags & IMM8)  len += 1;
  if (opcodes[i].flags & IMM16) len += 2;
  if (opcodes[i].flags & IMM32) len += 4;

  return len;
}

static void _sampgdk_hook_write_jmp(void *src,
                                    void *dst,
                                    _sampgdk_hook_int32_t offset) {
  struct _sampgdk_hook_jmp jmp;

  jmp.opcode = 0xE9;
  jmp.offset = (unsigned char *)dst - (
               (unsigned char *)src + sizeof(jmp));

  memcpy((unsigned char *)src + offset, &jmp, sizeof(jmp));
}

sampgdk_hook_t sampgdk_hook_new(void *src, void *dst) {
  struct _sampgdk_hook *hook;
  size_t orig_size = 0;
  size_t insn_len;

  if ((hook = malloc(sizeof(*hook))) == NULL) {
    return NULL;
  }

  _sampgdk_hook_unprotect(src, _SAMPGDK_HOOK_JMP_SIZE);
  _sampgdk_hook_unprotect(hook->trampoline, _SAMPGDK_HOOK_TRAMPOLINE_SIZE);

  /* We can't just jump to src + 5 as we could end up in the middle of
   * some instruction. So we need to determine the instruction length.
   */
  while (orig_size < _SAMPGDK_HOOK_JMP_SIZE) {
    unsigned char *code = (unsigned char *)src + orig_size;
    struct _sampgdk_hook_jmp *maybe_jmp;

    if ((insn_len = _sampgdk_hook_get_insn_len(code)) == 0) {
      sampgdk_log_error("Unsupported instruction");
      break;
    }

    memcpy(hook->trampoline + orig_size, code, insn_len);
    maybe_jmp = (struct _sampgdk_hook_jmp *)(hook->trampoline + orig_size);

    /* If the original code contains a relative JMP/CALL relocate its
     * destination address.
     */
    if (maybe_jmp->opcode == 0xE8 || maybe_jmp->opcode == 0xE9) {
      maybe_jmp->offset -= (_sampgdk_hook_int32_t)hook->trampoline
                         - (_sampgdk_hook_int32_t)src;
    }

    orig_size += insn_len;
  }

  if (insn_len > 0) {
    _sampgdk_hook_write_jmp(hook->trampoline, src, orig_size);
    _sampgdk_hook_write_jmp(src, dst, 0);
  } else {
    _sampgdk_hook_write_jmp(hook->trampoline, src, 0);
  }

  return hook;
}

void sampgdk_hook_free(sampgdk_hook_t hook) {
  free(hook);
}

void *sampgdk_hook_trampoline(sampgdk_hook_t hook) {
  return hook->trampoline;
}
