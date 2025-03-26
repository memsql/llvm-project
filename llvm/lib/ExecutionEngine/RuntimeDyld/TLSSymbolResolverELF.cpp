//===-- TLSSymbolResolverELF.cpp ----------------------------=---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "RuntimeDyldELF.h"
#include <future>

namespace llvm {

// The purpose of these classes is to support the implementation
// specific details of Thread Local Storage, which can differ
// between operating systems platforms and even C libraries.

#include "pthread.h"

// The DTV as specified in the ELF ABI
typedef union dtv {
    size_t counter;
    struct {
        void *val;
        bool _;
    } pointer;
} dtv_t;

typedef struct tcb_t {
    struct tcb *self;
    dtv_t *dtv;
} tcb_t;

// To perform local-exec relocations, we need to know the offset at which a
// given TLS symbol should be found in each thread's TLS array. This offset is
// chosen when compiling the executable defining these vars, and is the same for
// every thread. This information is present in ELF, but we take a shortcut for
// JIT compilation. The TLS offset for each var is static for a given
// executable, and since we assume that the executable running this function
// (memsqld) is the same as the executable of the process that's going to
// ultimately load our JIT-compiled code, we can just compute the offset of
// thread_var dynamically as &thread_var - base_of_tls_array, as it occurs in
// the thread running this code. The base of the tls array ("thread pointer")
// gets set by glibc in sysdeps/<arch>/nptl/tls.h.
inline uint64_t GetTLSBase() {
  uint64_t tls_base;
#ifdef __x86_64__
  asm ("movq %%fs:0, %0" : "=r" (tls_base));
#elif defined(__aarch64__)
  asm ("mrs %0, TPIDR_EL0" : "=r" (tls_base));
#endif
  return tls_base;
}

RuntimeDyld::TLSSymbolInfo
TLSSymbolResolverGLibCELF::findTLSSymbol(const std::string &Name) const {
    // It would be lovely to have an API for this. If we
    // wanted to, we might be able to actually look at the
    // internal libc datastructures, but that seems risky if
    // they were to change between versions. This implementation
    // is sketchy because it just searches through all the modules
    // and sees which one is closest, but at least it only relies on
    // the libc ABI, which should be stable.

    // First ask the MemoryManager to dlsym this value for us
    auto Sym = SR->findSymbol(Name);
    auto Addr = Sym.getAddress();
    if(!Addr)
        handleAllErrors(Addr.takeError());

    uint64_t Value = Addr.get();

    // Only enable this on x86_64, as this code was made for x86_64. This was
    // from the initial diff to LLVM that never landed. We only need to handle
    // local-exec, so we don't need this.
    //
    // This is dead code related to non-local-exec TLS models, where computing a
    // TLS var's address requires knowing about how the TLS var's .so was
    // loaded. Drew copied it from an llvm-project diff that was trying to
    // support more than local-exec. We're leaving this on for x86 because we
    // don't want to test this claim, but not trying to implement it for aarch64
    //
    // This is also non-trivial to handle on aarch64.
    // There are two variants of thread-local data structures:
    // https://uclibc.org/docs/tls.pdf. x86_64 is variant II, but aarch64 is
    // variant I. This means that on aarch64, the TCB is at the end DTV whereas
    // it is at the begining on x86_64. Also, the offset of the DTV from the TCB
    // seems to be inconsistent based off the implementation.
#ifdef __x86_64__
    const void  *UnallocatedDTVSlot = (void *)-1l;

    // This is glibc specifc but followed by a number of other C libraries
    tcb_t *tcb = (tcb_t *)pthread_self();
    dtv_t *dtv = tcb->dtv;

    // The number of allocated entries in the DTV is specified as the value
    // of dtv[-1]
    size_t cnt = dtv[-1].counter;

    // Find the module whose start block for the current thread is closest
    // to the dlsym'd address. Ugly, but works.
    uint64_t min_distance = (uint64_t)-1;
    uint64_t found_i = 0, found_offset = 0;
    for (size_t i = 1; i < cnt; ++i) {
      uint64_t distance = (Value - (uint64_t)dtv[i].pointer.val);
      if (dtv[i].pointer.val == UnallocatedDTVSlot) {
        continue;
      } else if ((uint64_t)dtv[i].pointer.val > Value) {
        continue;
      } else if (distance < min_distance) {
        min_distance = distance;
        found_i = i;
        found_offset = distance;
      }
    }
    assert(found_i != 0 && "Value could not be found in thread local storage");
#endif

    // Note: If this TLS symbol is not defined by the main application,
    // exec_distance will be some invalid garbage value.
    uint64_t tls_base = GetTLSBase();
    int64_t exec_distance = static_cast<int64_t>(Value - tls_base);

#ifdef __x86_64__
    RuntimeDyldELF::TLSSymbolInfoELF::ModuleInfo mod(found_i,
        (uint64_t)dtv[found_i].pointer.val - (uint64_t)tcb);
    return RuntimeDyldELF::TLSSymbolInfoELF(mod, found_offset, exec_distance).getOpaque();
#elif defined(__aarch64__)
    return RuntimeDyldELF::TLSSymbolInfoELF(RuntimeDyldELF::TLSSymbolInfoELF::ModuleInfo(0, 0), 0, exec_distance).getOpaque();
#endif
}

}
