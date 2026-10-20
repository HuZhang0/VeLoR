# VeLoR

VeLoR is a Verification-oriented Logic Representation and parallel Combinational Equivalence Checking (CEC) framework, built on top of Alice, mockturtle, and multiple SAT solvers.

## Highlights

- **Verification-oriented logic representation**: evaluates solver cost across NPN classes of 2–3 input Boolean functions to construct a verification-oriented heterogeneous cell library, and transforms circuits under verification-aware constraints for more efficient CNF encodings
- **Constraint-driven parallel solving**: identifies key variables based on verification complexity to guide task partitioning, enabling parallel CEC on multicore architectures
- **Multiple SAT backends**: Kissat, Glucose, MiniSat, and CaDiCaL
- **Flexible CNF generation**: default (mockturtle) flow and VeLoR (VLR) flow
- **Multiple network formats**: AIG, XMG, KLUT

## Build

```bash
git clone --recurse-submodules https://github.com/HuZhang0/VeLoR.git
cd velor
cmake -S . -B build
cmake --build build -j$(nproc)
```

Requirements: CMake 3.15+, a C++17 compiler, POSIX environment. The binary is written to `build/bin/velor`.

## Usage

```bash
./build/bin/velor
```

```text
velor> read_aiger example.aig
velor> vlr -a -g src/sat_lib/sat.genlib   # map AIG into VeLoR
velor> kissat -p 8 -C vlr -l   # parallel CEC with 8 processes, VLR-based CNF
velor> glucose -v
velor> minisat -v
velor> cadical -v
velor> vlr
```

### Commands

| Command | Description |
|---------|-------------|
| `read_aiger` / `read_blif` / `read_verilog` | Load AIG / BLIF(KLUT) / Verilog designs |
| `kissat` / `glucose` / `minisat` / `cadical` | Parallel CEC flows |
| `vlr` | VLR mapping and CNF generation (`-g` genlib, `-k` max cut size, `-e` engine, `-a` area-oriented) |
| `eval_cost` | Evaluate mapping cost and gate distribution |
| `lut_mapping` | LUT mapping utilities |

Common CEC options: `-p/--procs` number of processes (1 = sequential), `-s/--split` split variables, `-C/--cnf_method` CNF generation method (`mockturtle` or `vlr`), `-v` verbose output, `-c` show counterexample.

## Third-Party Libraries

Alice (command shell), mockturtle (logic synthesis and CNF utilities), kitty, lorina, Kissat, Glucose, MiniSat, and CaDiCaL, vendored under `lib/`.

## Repository Layout

```text
velor/
├── benchmarks/  benchmark circuits
├── lib/         third-party dependencies
├── src/         source code (command/, core/ parallel CEC engine, sat_lib/)
├── test/        tests
└── LICENSE      MIT license
```

## Citation

If you use VeLoR in your research, please cite our work published in ICCAD'26:

```bibtex
@inproceedings{hu2026VeLoR,
  title={VeLoR: Verification-Oriented Logic Representation and Parallel Solving for Combinational Equivalence Checking},
  author={Zhang Hu, Wentao Jiang, Chengyu Ma, Yinshui Xia, Lunyao Wang, and Zhufei Chu},
  booktitle={ICCAD},
  year={2026},
}
```

## License

VeLoR is distributed under the MIT License. See [LICENSE](LICENSE) for details.