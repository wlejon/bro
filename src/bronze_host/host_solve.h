#pragma once

#include "embed/embed.h"

namespace bro::bronze_host {

// Attaches the symbolic runtime & formal verification APIs onto `broObj`:
//   - `bro.solve(fn, opts)` / `bro.solveAll(fn, maxSolutions, opts)` / `bro.solveWithCert(fn, opts)` (#128)
//   - `bro.optimize(fn, opts)` (#129)
//   - `bro.fuzz(fn, opts)` (#133)
//   - `bro.prove(fn, opts)` (#135)
//   - `bro.layout.solve(specOrVars, constraintFnOrOpts, opts)` (#137)
//   - `bro.superoptimize(fnOrSource, opts)` (#142)
//   - `bro.rewriteSource(fnOrSource, opts)` (#143)
//   - `bro.autoKernel(fnOrSource, opts)` (#144)
void attachBroSolveApis(bronze::Value broObj);
void attachBroLayoutApis(bronze::Value broObj);
void attachBroRewriteApis(bronze::Value broObj);

}  // namespace bro::bronze_host
