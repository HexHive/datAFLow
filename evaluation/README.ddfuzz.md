# DDFuzz Targets

[DDFuzz](https://doi.org/10.1109/EuroSP53844.2022.00026) evaluated against the
following five targets the authors thought would be amenable to data-flow-based
fuzzing:

* [bison](https://github.com/akimd/bison/tree/5555f4d05163316b8b5bddbdb172c0f5bae6f765)
* [pcre2](https://github.com/PCRE2Project/pcre2/tree/db53e4007db5e1fcfa144ddf10d0499a111a770b)
* [mir](https://github.com/vnmakarov/mir/tree/852b1f2e226001f355008004cc5ec2398889becc)
* [qbe](https://c9x.me/git/qbe.git/tree/?id=c8cd2824eae0137505fe46530c3a8e9788ab9a63)
* [faust](https://github.com/grame-cncm/faust/tree/13def69f21dd0cede393c13dcaf1a6fe7ef7f439)

The links above are to the commit hashes used in the paper.

## AFL++

We instrument the targets with AFL++'s [LTO](https://github.com/AFLplusplus/AFLplusplus/blob/stable/instrumentation/README.lto.md)
instrumentation and with [CmpLog](https://github.com/AFLplusplus/AFLplusplus/blob/stable/instrumentation/README.cmplog.md)
instrumentation.

When fuzzing we disable the auto-dictionary with `AFL_NO_AUTODICT=1`.

## Angora

We use Angora per their [instructions](https://github.com/AngoraFuzzer/Angora).

## DDFuzz

We use DDFuzz per their [instructions](https://github.com/elManto/DDFuzz).

## datAFLow

We instrument with different datAFLow configurations:

* "datAFLow (A/A)"

```
FUZZALLOC_DEF_SENSITIVITY=array
FUZZALLOC_USE_SENSITIVITY=read:write
FUZZALLOC_USE_CAPTURE=use
FUZZALLOC_USE_INST=afl
```

* "datAFLow (A/O)"

```
FUZZALLOC_DEF_SENSITIVITY=array
FUZZALLOC_USE_SENSITIVITY=read:write
FUZZALLOC_USE_CAPTURE=offset
FUZZALLOC_USE_INST=afl
```

* "datAFLow (A/V)"

```
FUZZALLOC_DEF_SENSITIVITY=array
FUZZALLOC_USE_SENSITIVITY=read:write
FUZZALLOC_USE_CAPTURE=value
FUZZALLOC_USE_INST=afl
```

* "datAFLow (A+S/A)"

```
FUZZALLOC_DEF_SENSITIVITY=array:struct
FUZZALLOC_USE_SENSITIVITY=read:write
FUZZALLOC_USE_CAPTURE=use
FUZZALLOC_USE_INST=afl
```

* "datAFLow (A+S/O)"

```
FUZZALLOC_DEF_SENSITIVITY=array:struct
FUZZALLOC_USE_SENSITIVITY=read:write
FUZZALLOC_USE_CAPTURE=offset
FUZZALLOC_USE_INST=afl
```

* "datAFLow (A+S/V)"

```
FUZZALLOC_DEF_SENSITIVITY=array:struct
FUZZALLOC_USE_SENSITIVITY=read:write
FUZZALLOC_USE_CAPTURE=value
FUZZALLOC_USE_INST=afl
```

We also use the custom memory allocation functions defined in
`ddfuzz/mem-funcs`, specified using the `FUZZALLOC_DEF_MEM_FUNCS` environment
variable.
