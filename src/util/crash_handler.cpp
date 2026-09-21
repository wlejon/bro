// See crash_handler.h.
//
// Everything in the handler path is written to survive a faulted process: no
// heap allocation, no locks, no C++ exceptions, no iostreams. The context
// breadcrumb is a fixed char buffer rather than a std::string for the same
// reason — reading a std::string whose heap the crash just corrupted is how a
// crash handler turns one stack trace into none.

#include "util/crash_handler.h"

#include <atomic>
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

void walkHere() {}

#endif  // _WIN32

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
#ifdef _WIN32
    if (g_dumps.fetch_add(1) < kMaxDumps) walkHere();
#endif
    std::fprintf(stderr, "=== end crash ===\n");
    std::fflush(stderr);
    // Re-raise through the default disposition so the process dies with the
    // status it would have died with anyway.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

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
#endif

    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGILL, signalHandler);
    std::signal(SIGFPE, signalHandler);
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
#ifdef _WIN32
    walkHere();
#endif
    std::fprintf(stderr, "=== end backtrace ===\n");
    std::fflush(stderr);
}

} // namespace bro::util
