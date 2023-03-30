# datAFLow

<p>
<a href="https://www.ndss-symposium.org/wp-content/uploads/fuzzing2022_23001_paper.pdf" target="_blank">
<img align="right" width="250" src="img/dataflow-paper.png">
</a>
</p>

DatAFLow is a fuzzer built on top of
[AFL++](https://github.com/AFLplusplus/AFLplusplus/). However, instead of a
control-flow-based feedback mechanism (e.g., based on control-flow edge
coverage), datAFLow uses a data-flow-based feedback mechanism; specifically,
data flows based on _def-use associations_.

To enable performant fuzzing, datAFLow uses a flexible and efficient memory
object metadata scheme based on the "Padding Area MetaData"
([PAMD](https://dl.acm.org/doi/10.1145/3156685.3092268)) approach.

More details are available in our registered report, published at the [1st
International Fuzzing Workshop (FUZZING)
2022](https://fuzzingworkshop.github.io/). You can read our report
[here](https://www.ndss-symposium.org/wp-content/uploads/fuzzing2022_23001_paper.pdf).

## Requirements

datAFLow is built on LLVM v12. Python is also required (for the `dataflow-cc`
wrapper).

### Building `z3`

Z3 is required by [SVF](https://github.com/svf-tools/svf) (for static analysis).
If running datAFLow on Ubuntu 20.04, you can install z3 via `apt`.

```bash
git clone https://github.com/z3prover/z3
git -C z3 checkout z3-4.8.8
mkdir -p z3/build
cd z3/build
cmake .. \
    -DCMAKE_INSTALL_PREFIX=$(realpath ../install) -DZ3_BUILD_LIBZ3_SHARED=False
make -j
make install
```

### Building `fuzzalloc`

`FUZZALLOC_SRC` variable refers to this directory (i.e., the root source
directory). Ensure all submodules are initialized.

```bash
cd $FUZZALLOC_SRC
git submodule update --init --recursive
```

Then build. The `Z3_DIR` option is only required of z3 was built from source.

```bash
cd $FUZZALLOC_SRC
mkdir build
cd build
cmake .. \
    -DCMAKE_C_COMPILER=clang-12 -DCMAKE_CXX_COMPILER=clang++-12 \
    -DLLVM_DIR=$(llvm-config-12 --cmakedir) \
    -DZ3_DIR=/path/to/z3/install
make -j
```

## Instrumenting a Target

The `dataflow-cc` (and `dataflow-cc++`) tools can be used as dropin replacements
for `clang` (and `clang++`).  These wrappers provide a number of environment
variables to configure the target:

* `FUZZALLOC_DEF_MEM_FUNCS`: Path to a special case list (see below) listing
custom memory allocation routines

* `FUZZALLOC_DEF_SENSITIVITY`: The def sites to instrument. One of `array`,
`struct`, or `array:struct`.

* `FUZZALLOC_USE_SENSITIVITY`: The use sites to instrument. One of `read`,
`write`, or `read:write`.

* `FUZZALLOC_USE_CAPTURE`: What to capture at each use site. One of `use`,
`offset`, or `value`.

* `FUZZALLOC_INST`: Instrumentation. One of: `afl` (for fuzzing); `tracer` (for
accurate tracing of def-use chains); or `none`.

### Custom memory allocators

If the target uses custom memory allocation routines (i.e., wrapping `malloc`,
`calloc`, etc.), then a [special case
list](https://clang.llvm.org/docs/SanitizerSpecialCaseList.html) containing a
list of these routines should be provided to `dataflow-preprocess`. Doing so
ensures dynamically-allocated variable _def_ sites are appropriately tagged. The
list is provided via the `--def-mem-funcs` option. The special case list must be
formatted as:

```
[fuzzalloc]
fun:malloc_wrapper
fun:calloc_wrapper
fun:realloc_wrapper
```

## Tools

In addition to `dataflow-cc` and `dataflow-c++`, we provide the following tools:

### `static-dua`

Uses [SVF](https://github.com/SVF-tools/SVF/) to statically derive an upper
bounds on the number of def-use chains in a BC file. This tool generates JSON
output tying these def-use chains to source-level variables (recovered through
debug info).

Note that you must run CMake with the `-DUSE_SVF=On` option to build this tool.

### `dataflow-stats`

Collect `fuzzalloc` stats from an instrumented bitcode file. Stats include:
number of tagged variables, number of instrumented use sites, etc.

### `static-region-cov`

`static-region-cov` statically extracts Clang's [source-based code
coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html) from an
instrumented binary.

### `dua-cov-json`

Generate data-flow coverage over time from an AFL++ queue output directory.
Relies on a version of the target program instrumented with trace mode (i.e.,
setting `FUZZALLOC_INST=trace`) to replay the queue through, generating JSON
reports logging covered def-use chains.

### `llvm-cov-json`

Generate control-flow coverage over time from an AFL++ queue output directory.
Relies on a version of the target program instrumented with Clang's source-based
coverage (i.e., compiled using Clang's `-fprofile-instr-generate
-fcoverage-mapping` flags) to replay the queue through, generating JSON reports
logging covered def-use chains.

# Evaluation Reproduction

See [README.magma.md](evaluation/README.magma.md) and
[README.ddfuzz.md](evaluation/README.ddfuzz.md) for reproducing the Magma and
DDFuzz experiments, respectively.
