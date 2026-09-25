// See crash_handler.h.
//
// Everything in the handler path is written to survive a faulted process: no
// heap allocation, no locks, no C++ exceptions, no iostreams. The context
// breadcrumb is a fixed char buffer rather than a std::string for the same
// reason — reading a std::string whose heap the crash just corrupted is how a
// crash handler turns one stack trace into none.

#include "util/crash_handler.h"

#include <atomic>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <dlfcn.h>
#include <signal.h>
// <ucontext.h> is an #error on Apple without _XOPEN_SOURCE; the struct itself
// lives in the sys/ header there.
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define BRO_HAVE_EXECINFO 1
#endif
#endif

namespace bro::util {

namespace {

constexpr int kContextCap = 512;
char g_context[kContextCap] = {0};

std::atomic<int> g_dumps{0};
std::atomic<bool> g_installed{false};

// At most a handful of dumps: a fault inside the handler, or a guard-page
// pattern that fires repeatedly, must not turn a crash report into a
// gigabyte of scrollback.
constexpr int kMaxDumps = 4;

#ifdef _WIN32

std::atomic<bool> g_symsReady{false};

void ensureSymbols() {
    if (g_symsReady.exchange(true)) return;
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                  SYMOPT_LOAD_LINES);
    // Search next to the executable first: that is where the build stages the
    // PDBs, and a Release build without them symbolises to module+rva only.
    char exePath[MAX_PATH] = {0};
    char* search = nullptr;
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        char* slash = std::strrchr(exePath, '\\');
        if (slash) {
            *slash = '\0';
            search = exePath;
        }
    }
    SymInitialize(GetCurrentProcess(), search, TRUE);
}

const char* moduleBase(DWORD64 addr, char* buf, size_t cap, DWORD64& rva) {
    rva = 0;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) ||
        !mod) {
        return "?";
    }
    rva = addr - reinterpret_cast<DWORD64>(mod);
    if (GetModuleFileNameA(mod, buf, static_cast<DWORD>(cap)) == 0) return "?";
    const char* base = std::strrchr(buf, '\\');
    return base ? base + 1 : buf;
}

// The addresses go out FIRST, from the loader's own module list, and only
// then does anything ask DbgHelp for a name. The order is deliberate: a
// process that faulted during teardown can take SymInitialize / SymFromAddr
// down with it, and a handler that symbolises as it walks then prints a
// header, an empty backtrace and nothing else — which is exactly what the
// first version of this did. Addresses plus module+rva are always available
// and are already enough to place a frame against a map file.
void printAddr(unsigned index, DWORD64 addr) {
    char modBuf[MAX_PATH] = {0};
    DWORD64 rva = 0;
    const char* mod = moduleBase(addr, modBuf, sizeof(modBuf), rva);
    std::fprintf(stderr, "  #%02u 0x%016llx %s+0x%llx\n", index,
                 static_cast<unsigned long long>(addr), mod,
                 static_cast<unsigned long long>(rva));
}

void symbolisePass(const DWORD64* addrs, unsigned n) {
    HANDLE proc = GetCurrentProcess();
    std::fprintf(stderr, "symbols:\n");
    for (unsigned i = 0; i < n; ++i) {
        char symBuf[sizeof(SYMBOL_INFO) + 512] = {0};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        const char* name =
            SymFromAddr(proc, addrs[i], &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line;
        std::memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addrs[i], &lineDisp, &line)) {
            std::fprintf(stderr, "  #%02u %s+0x%llx (%s:%lu)\n", i, name,
                         static_cast<unsigned long long>(disp), line.FileName,
                         static_cast<unsigned long>(line.LineNumber));
        } else {
            std::fprintf(stderr, "  #%02u %s+0x%llx\n", i, name,
                         static_cast<unsigned long long>(disp));
        }
    }
}

// DbgHelp is not required to survive the process state a crash handler runs
// in, so its whole contribution sits behind an exception wall. Losing the
// names must not lose the trace.
void symboliseGuarded(const DWORD64* addrs, unsigned n) {
    __try {
        ensureSymbols();
        symbolisePass(addrs, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "  (symbolisation failed; addresses above)\n");
    }
}

// Unwind from an explicit CONTEXT — the FAULTING frames, not the handler's
// own, which is what CaptureStackBackTrace would give and is never the
// interesting place.
//
// On x64 the walk is RtlLookupFunctionEntry + RtlVirtualUnwind rather than
// DbgHelp's StackWalk64. Same answer when both work, but StackWalk64 goes
// through SymFunctionTableAccess64, so a process whose SymInitialize did not
// take — a 300 MB PDB not yet paged in, a module list DbgHelp has not caught
// up with — walks ZERO frames and prints an empty backtrace, which is the one
// outcome a crash handler must not have. The unwind tables are in the loaded
// image and always present; DbgHelp is then only asked to name addresses,
// and a failure there costs a name, not the trace.
void walk(CONTEXT* ctxIn) {
    DWORD64 addrs[64];
    unsigned n = 0;
    std::fprintf(stderr, "backtrace:\n");

#if defined(_M_X64)
    CONTEXT ctx = *ctxIn;
    while (n < 64) {
        if (ctx.Rip == 0) break;
        addrs[n] = ctx.Rip;
        printAddr(n, ctx.Rip);
        ++n;

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase,
                                                      nullptr);
        if (fn == nullptr) {
            // A leaf function, or code with no unwind data (a JIT thunk).
            // The return address is the top of the stack.
            if (ctx.Rsp == 0) break;
            DWORD64 ret = 0;
            std::memcpy(&ret, reinterpret_cast<const void*>(ctx.Rsp),
                        sizeof(ret));
            ctx.Rip = ret;
            ctx.Rsp += sizeof(DWORD64);
            continue;
        }
        PVOID handlerData = nullptr;
        ULONG64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                         &handlerData, &establisher, nullptr);
    }
#else
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    STACKFRAME64 frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset    = ctxIn->Eip;
    frame.AddrFrame.Offset = ctxIn->Ebp;
    frame.AddrStack.Offset = ctxIn->Esp;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Mode   = AddrModeFlat;
    ensureSymbols();
    while (n < 64) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, thread, &frame, ctxIn,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;
        addrs[n] = frame.AddrPC.Offset;
        printAddr(n, frame.AddrPC.Offset);
        ++n;
    }
#endif
    if (n > 0) symboliseGuarded(addrs, n);
}

void walkHere() {
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);
    walk(&ctx);
}

const char* codeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        default:                              return "exception";
    }
}

// The codes worth a report. Everything else — a C++ throw (0xE06D7363), a
// debugger's breakpoint, the CLR's and the JIT's own signalling — is normal
// traffic through a vectored handler and must pass straight through.
bool fatalCode(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return true;
        default:
            return false;
    }
}

void report(const char* how, EXCEPTION_POINTERS* ep) {
    std::fflush(stdout);
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    std::fprintf(stderr,
                 "\n=== bro-headless crash (%s) ===\n"
                 "  code    0x%08lX  %s\n"
                 "  address 0x%016llx\n"
                 "  thread  %lu\n",
                 how, static_cast<unsigned long>(r->ExceptionCode),
                 codeName(r->ExceptionCode),
                 reinterpret_cast<unsigned long long>(r->ExceptionAddress),
                 static_cast<unsigned long>(GetCurrentThreadId()));
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        r->NumberParameters >= 2) {
        const ULONG_PTR op = r->ExceptionInformation[0];
        std::fprintf(stderr, "  %s address 0x%016llx\n",
                     op == 0 ? "reading" : (op == 1 ? "writing" : "executing"),
                     static_cast<unsigned long long>(
                         r->ExceptionInformation[1]));
    }
    if (g_context[0] != '\0') {
        std::fprintf(stderr, "  running %s\n", g_context);
    }
    walk(ep->ContextRecord);
    std::fprintf(stderr, "=== end crash ===\n");
    std::fflush(stderr);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    if (g_dumps.fetch_add(1) < kMaxDumps) report("unhandled", ep);
    std::fflush(stdout);
    std::fflush(stderr);
    // Go with the code the fault actually carried, so a caller reading the
    // exit status sees the same crash it would have seen without us.
    TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI vectoredHandler(EXCEPTION_POINTERS* ep) {
    // First-chance: print and get out of the way. A frame-based __except may
    // still be about to handle this legitimately (bronze's own eval wraps the
    // entry point in one), and swallowing it here would change behaviour to
    // buy nothing. The report is the point.
    if (fatalCode(ep->ExceptionRecord->ExceptionCode) &&
        g_dumps.fetch_add(1) < kMaxDumps) {
        report("first chance", ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#else   // !_WIN32

// One address as module+offset and, when the dynamic symbol table has it,
// symbol+offset. dladdr is not on POSIX's async-signal-safe list, but it only
// reads the loader's image list — backtrace_symbols_fd uses it the same way —
// and a crash report that names nothing is the worse failure.
void printAddr(const char* label, uintptr_t addr) {
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    if (addr != 0 && dladdr(reinterpret_cast<void*>(addr), &info) != 0 &&
        info.dli_fname != nullptr) {
        const char* mod = std::strrchr(info.dli_fname, '/');
        mod = mod ? mod + 1 : info.dli_fname;
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
        if (info.dli_sname != nullptr) {
            const uintptr_t sym = reinterpret_cast<uintptr_t>(info.dli_saddr);
            std::fprintf(stderr, "  %-7s 0x%016llx %s+0x%llx (%s+0x%llx)\n",
                         label, static_cast<unsigned long long>(addr), mod,
                         static_cast<unsigned long long>(addr - base),
                         info.dli_sname,
                         static_cast<unsigned long long>(addr - sym));
        } else {
            std::fprintf(stderr, "  %-7s 0x%016llx %s+0x%llx\n", label,
                         static_cast<unsigned long long>(addr), mod,
                         static_cast<unsigned long long>(addr - base));
        }
        return;
    }
    // Not in any loaded image: JIT'd or AOT-loaded code, or a wild jump.
    std::fprintf(stderr, "  %-7s 0x%016llx (no image)\n", label,
                 static_cast<unsigned long long>(addr));
}

// The calling thread's stack, from a signal handler or not. backtrace() walks
// frame pointers (always kept on arm64 macOS), so from a handler the first
// frames are the handler's own and the kernel's trampoline, then the
// interrupted code; the faulting pc itself is printed separately from the
// ucontext, because a fault in a leaf never pushed a frame for it.
void walkHere() {
    std::fprintf(stderr, "backtrace:\n");
#ifdef BRO_HAVE_EXECINFO
    void* addrs[64];
    const int n = backtrace(addrs, 64);
    // backtrace_symbols_fd, not backtrace_symbols: it writes straight to the
    // fd and never calls malloc, whose arena the crash may have wrecked.
    backtrace_symbols_fd(addrs, n, STDERR_FILENO);
#else
    std::fprintf(stderr, "  (no execinfo on this platform)\n");
#endif
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        default:      return "signal";
    }
}

const char* codeName(int sig, int code) {
    if (sig == SIGSEGV) {
        if (code == SEGV_MAPERR) return "SEGV_MAPERR (address not mapped)";
        if (code == SEGV_ACCERR) return "SEGV_ACCERR (permission denied)";
    } else if (sig == SIGBUS) {
        if (code == BUS_ADRALN) return "BUS_ADRALN (misaligned)";
        if (code == BUS_ADRERR) return "BUS_ADRERR (nonexistent address)";
        if (code == BUS_OBJERR) return "BUS_OBJERR (object error)";
    } else if (sig == SIGILL) {
        if (code == ILL_ILLOPC) return "ILL_ILLOPC (illegal opcode)";
        if (code == ILL_ILLTRP) return "ILL_ILLTRP (illegal trap)";
        if (code == ILL_PRVOPC) return "ILL_PRVOPC (privileged opcode)";
    } else if (sig == SIGFPE) {
        if (code == FPE_INTDIV) return "FPE_INTDIV (integer divide by zero)";
        if (code == FPE_INTOVF) return "FPE_INTOVF (integer overflow)";
    }
    return "";
}

// The interrupted thread's registers, per platform and architecture. The pc
// is the instruction that faulted; on arm64 the link register is where a
// leaf would have returned to, and the fault address register is what the
// access actually touched (si_addr carries it too, but FAR is the hardware's
// own word for it, and ESR says what kind of access it was).
void printRegisters(void* uctx) {
    if (uctx == nullptr) return;
    auto* uc = static_cast<ucontext_t*>(uctx);
#if defined(__APPLE__) && defined(__aarch64__)
    const auto& ss = uc->uc_mcontext->__ss;
    const auto& es = uc->uc_mcontext->__es;
    printAddr("pc", static_cast<uintptr_t>(__darwin_arm_thread_state64_get_pc(ss)));
    printAddr("lr", static_cast<uintptr_t>(__darwin_arm_thread_state64_get_lr(ss)));
    std::fprintf(stderr, "  sp      0x%016llx  fp 0x%016llx\n",
                 static_cast<unsigned long long>(__darwin_arm_thread_state64_get_sp(ss)),
                 static_cast<unsigned long long>(__darwin_arm_thread_state64_get_fp(ss)));
    std::fprintf(stderr, "  far     0x%016llx  esr 0x%08x\n",
                 static_cast<unsigned long long>(es.__far),
                 static_cast<unsigned>(es.__esr));
#elif defined(__APPLE__) && defined(__x86_64__)
    const auto& ss = uc->uc_mcontext->__ss;
    printAddr("rip", static_cast<uintptr_t>(ss.__rip));
    std::fprintf(stderr, "  rsp     0x%016llx  rbp 0x%016llx\n",
                 static_cast<unsigned long long>(ss.__rsp),
                 static_cast<unsigned long long>(ss.__rbp));
    std::fprintf(stderr, "  fault   0x%016llx\n",
                 static_cast<unsigned long long>(uc->uc_mcontext->__es.__faultvaddr));
#elif defined(__linux__) && defined(__x86_64__)
    printAddr("rip", static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]));
    std::fprintf(stderr, "  rsp     0x%016llx  rbp 0x%016llx\n",
                 static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RSP]),
                 static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RBP]));
#elif defined(__linux__) && defined(__aarch64__)
    printAddr("pc", static_cast<uintptr_t>(uc->uc_mcontext.pc));
    printAddr("lr", static_cast<uintptr_t>(uc->uc_mcontext.regs[30]));
    std::fprintf(stderr, "  sp      0x%016llx  fp 0x%016llx\n",
                 static_cast<unsigned long long>(uc->uc_mcontext.sp),
                 static_cast<unsigned long long>(uc->uc_mcontext.regs[29]));
    std::fprintf(stderr, "  far     0x%016llx\n",
                 static_cast<unsigned long long>(uc->uc_mcontext.fault_address));
#else
    (void)uc;
#endif
}

void signalHandler(int sig, siginfo_t* info, void* uctx) {
    std::fflush(stdout);
    std::fprintf(stderr, "\n=== bro-headless crash (%s) ===\n", signalName(sig));
    if (info != nullptr && sig != SIGABRT) {
        std::fprintf(stderr, "  code    %d %s\n", info->si_code,
                     codeName(sig, info->si_code));
        std::fprintf(stderr, "  address 0x%016llx\n",
                     static_cast<unsigned long long>(
                         reinterpret_cast<uintptr_t>(info->si_addr)));
    }
    if (g_context[0] != '\0') {
        std::fprintf(stderr, "  running %s\n", g_context);
    }
    if (g_dumps.fetch_add(1) < kMaxDumps) {
        printRegisters(uctx);
        walkHere();
    }
    std::fprintf(stderr, "=== end crash ===\n");
    std::fflush(stderr);
    // SA_RESETHAND already put the default disposition back. A hardware fault
    // re-executes the faulting instruction on return and dies of it with the
    // status it would have had anyway; raise() covers a signal that was sent
    // rather than caused (kill -SEGV, abort()'s own raise).
    std::raise(sig);
}

// The alternate stack a stack overflow's SIGSEGV/SIGBUS runs on: the faulting
// thread's own stack is the guard page it just ran into, and a handler that
// needs stack there never runs at all. Static, because a crash handler is no
// place to find out the heap is gone. Per-thread by POSIX, so it covers the
// thread that installs it (main, where the JS runs); another thread that
// overflows still dies, just without the report.
alignas(16) char g_altStack[128 * 1024];

void installSignals() {
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

#ifdef BRO_HAVE_EXECINFO
    // glibc's first backtrace() dlopens libgcc_s, which allocates. Do it now,
    // while allocating is still allowed.
    void* warm[1];
    backtrace(warm, 1);
#endif

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    // SIGBUS is macOS's word for most bad accesses into mapped-but-wrong
    // memory (a JIT page without execute, a truncated mmap), so it matters as
    // much as SIGSEGV there.
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) {
        sigaction(sig, &sa, nullptr);
    }
}

#endif  // _WIN32

#ifdef _WIN32
void signalHandler(int sig) {
    std::fflush(stdout);
    const char* name = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGABRT ? "SIGABRT"
                       : sig == SIGILL  ? "SIGILL"
                       : sig == SIGFPE  ? "SIGFPE"
                                        : "signal";
    std::fprintf(stderr, "\n=== bro-headless crash (%s) ===\n", name);
    if (g_context[0] != '\0') {
        std::fprintf(stderr, "  running %s\n", g_context);
    }
    if (g_dumps.fetch_add(1) < kMaxDumps) walkHere();
    std::fprintf(stderr, "=== end crash ===\n");
    std::fflush(stderr);
    // Re-raise through the default disposition so the process dies with the
    // status it would have died with anyway.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

} // namespace

void installCrashHandler() {
    if (g_installed.exchange(true)) return;

    // The whole reason the crashes were silent. A redirected stdout is fully
    // buffered by the CRT, so up to 4 KB of the last thing the run said dies
    // with it; stderr is only nominally unbuffered and is not reliable once a
    // native module has written to it. Unbuffered costs a syscall per write,
    // which against a 1.5 s render is free.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

#ifdef _WIN32
    // No modal "program has stopped working" box: a crashed headless run must
    // exit, not wait for a click that will never come.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(unhandledFilter);
    AddVectoredExceptionHandler(/*first=*/1, vectoredHandler);

    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGILL, signalHandler);
    std::signal(SIGFPE, signalHandler);
#else
    installSignals();
#endif
}

void setCrashContext(const std::string& what) {
    const std::size_t n =
        what.size() < static_cast<std::size_t>(kContextCap) - 1
            ? what.size()
            : static_cast<std::size_t>(kContextCap) - 1;
    std::memcpy(g_context, what.data(), n);
    g_context[n] = '\0';
}

void dumpBacktrace(const char* reason) {
    std::fflush(stdout);
    std::fprintf(stderr, "\n=== backtrace: %s ===\n",
                 reason ? reason : "requested");
    if (g_context[0] != '\0') {
        std::fprintf(stderr, "  running %s\n", g_context);
    }
    walkHere();
    std::fprintf(stderr, "=== end backtrace ===\n");
    std::fflush(stderr);
}

} // namespace bro::util
