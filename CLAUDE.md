# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PS2Recomp is a static recompiler that translates PlayStation 2 MIPS R5900 ELF binaries into C++ code for native execution on modern platforms. The project is experimental and inspired by N64Recomp.

## Build Commands

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/ran-j/PS2Recomp.git
cd PS2Recomp

# Build (requires CMake 3.20+, C++20 compiler)
mkdir build && cd build
cmake ..
cmake --build .

# Run tests
./ps2xTest/ps2x_tests      # Linux
.\build\ps2xTest\Release\ps2x_tests.exe   # Windows

# Run recompiler
./ps2recomp path/to/config.toml

# Run analyzer
./ps2_analyzer
```

### Build Targets
- `ps2recomp` - Main recompiler executable
- `ps2_analyzer` - ELF analysis tool for generating configs
- `ps2EntryRunner` - Runtime execution environment
- `ps2x_tests` - Test suite

## Architecture

The project is organized into four main components:

### 1. ps2xRecomp (Core Recompiler)
Contains the static recompilation engine. Key files:
- `src/lib/ps2_recompiler.cpp` - Main recompilation pipeline
- `src/lib/r5900_decoder.cpp` - MIPS R5900 instruction decoder
- `src/lib/code_generator.cpp` - C++ code generation from instructions
- `src/lib/elf_parser.cpp` - ELF file parsing using ELFIO
- `src/lib/config_manager.cpp` - TOML configuration handling

### 2. ps2xRuntime (Execution Environment)
Provides runtime support for executing recompiled code:
- PS2 memory simulation (32MB RAM, scratchpad, etc.)
- `R5900Context` with 128-bit GPRs using SSE/AVX intrinsics
- System call stubs and PS2 hardware simulation hooks

### 3. ps2xAnalyzer (Analysis Tool)
Analyzes PS2 ELF binaries to generate TOML configuration files, identifying functions, symbols, and recommending stubs/patches.

### 4. ps2xTest (Test Suite)
Unit tests for the decoder and code generator.

### Recompilation Pipeline
```
PS2 ELF → ElfParser → R5900Decoder → CodeGenerator → C++ output
                ↓
         Config Manager ← TOML config
```

## Key Data Structures (ps2xRecomp/include/ps2recomp/types.h)

- **Instruction** - MIPS instruction with PS2-specific metadata (MMI/VU flags, delay slot info, vector operations)
- **Function** - Code units with caller/callee relationship tracking
- **Symbol** - Function/data symbols with import/export flags
- **CFGNode** - Control flow graph nodes
- **R5900Context** - CPU state with 128-bit registers, VU0 state, COP0 registers

## Configuration System

TOML-based config (see `ps2xRecomp/example_config.toml`):

```toml
[general]
input = "path/to/game.elf"      # Input PS2 ELF
output = "output/"               # Output directory
single_file_output = false       # false = one file per function

stubs = ["printf", "malloc"]     # Generate empty implementations
skip = ["abort", "exit"]         # Don't recompile these functions

[patches]
instructions = [
  { address = "0x100004", value = "0x00000000" }  # Patch instruction
]
```

## Important PS2-Specific Concepts

- **128-bit GPRs**: PS2's R5900 has 128-bit general-purpose registers (using SSE/AVX intrinsics)
- **MMI Instructions**: PS2-specific multimedia instructions
- **VU0 Macro Mode**: Vector Unit 0 in macro mode requires specialized handling
- **Memory Regions**: KSEG0/KSEG1 address translation, 32MB main RAM, scratchpad
- **Delay Slots**: Branch/jump instructions execute the following instruction before taking effect

## Code Generation Style

The recompiler produces literal translations - each MIPS instruction maps to a C++ operation. Example:
- MIPS: `addiu $r4, $r4, 0x20`
- C++: `ctx->r4 = ADD32(ctx->r4, 0X20);`

## Platform Notes

- Primarily tested with MSVC on Windows
- Requires SSE4/AVX support for 128-bit register operations
- Linux compilation supported (see recent commits for ELF parser fixes)
