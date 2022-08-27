# Magma

[Magma](https://hexhive.epfl.ch/magma/) can be patched to support datAFLow as
follows:

```bash
git clone https://github.com/hexhive/magma
cd magma
git apply magma/magma.patch
```

Magma can then be run as usual. We provide a sample `captainrc` file that
reproduces the results in the paper.
