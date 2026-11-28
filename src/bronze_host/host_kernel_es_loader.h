#pragma once

namespace bronze::eval {
struct EvalOptions;
}

namespace bro::bronze_host {

// Registers `.bk` ES module loading on `opts`: importing `./foo.bk` in ES modules
// compiles the kernel module with `bro.kernel.compile`, tracks transitive `.bk`
// dependencies in `extraFilesRead`, and exports its kernels, `@fn` functions,
// and default `KernelModule`.
void configureKernelEsLoader(bronze::eval::EvalOptions& opts);

}  // namespace bro::bronze_host
