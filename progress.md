# Project Progress

## 04 October 2026

### What I have implemented
- Implemented and tested Stack data structure.
- Implemented Timeline data structure.
- Implemented `readSourceLine()`.
- Implemented `firstWord()` and `secondWord()`.
- Implemented Pass 0x0 `validateProgram()`.
- Added validation for nested functions and matching `func_end`.

### From where I will start tomorrow
- Start Pass 0x1 Resolve.
- Implement `resolve.bin` file handling.
- Implement function offset tracking and `call` resolution.

## 07 October 2026

### Pass 0x1 - Resolve
- Implemented `resolve.bin` generation.
- Stores records in the format:
  `[offset][string_size][string]`
- Stores function names and their offsets.
- Resolves `call` instructions to function offsets.
- Added validation for undefined functions.
- Added validation for duplicate functions.
- Added validation for missing `main`.
- Uses a fixed-size offset placeholder (`0x00000000`) so record sizes remain unchanged.

## Phase 0x2 - Execution
- Tokenization implemented.
- Snapshot structure planned/implemented.