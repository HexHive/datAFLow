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

Z3 is required if using [SVF](https://github.com/svf-tools/svf) for static
analysis.

```bash
git clone https://github.com/z3prover/z3
mkdir -p z3/build
cd z3/build
cmake .. \
    -DCMAKE_INSTALL_PREFIX=$(realpath ../install) -DZ3_BUILD_LIBZ3_SHARED=False
make -j
make install
```

### Building `fuzzalloc`

`FUZZALLOC_SRC` variable refers to this directory. Ensure all submodules are
initialized.

```bash
cd $FUZZALLOC_SRC
git submodule update --init --recursive
```

Then build.

```bash
mkdir build
cd build
cmake $FUZZALLOC_SRC \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DZ3_DIR=/path/to/z3/install
make -j
```

### Instrumenting a Target

The `dataflow-cc` (and `dataflow-cc++`) tools can be used as dropin replacements
for `clang` (and `clang++`).

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
