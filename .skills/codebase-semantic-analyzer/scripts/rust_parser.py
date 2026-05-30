#!/usr/bin/env python3
import sys
import re
import os

# Regular expressions to match Rust semantic elements
RE_STRUCT = re.compile(r'^\s*(?:pub\s+)?struct\s+(\w+)')
RE_ENUM = re.compile(r'^\s*(?:pub\s+)?enum\s+(\w+)')
RE_TRAIT = re.compile(r'^\s*(?:pub\s+)?trait\s+(\w+)')
RE_IMPL = re.compile(r'^\s*impl(?:\s*<.*>)?\s+(?:(\w+)\s+for\s+)?(\w+)')
RE_FN = re.compile(r'^\s*(?:pub\s+)?(?:async\s+)?fn\s+(\w+)')

def analyze_rust_file(filepath):
    if not os.path.exists(filepath):
        print(f"Error: File '{filepath}' does not exist.", file=sys.stderr)
        return

    print(f"=== Semantic Analysis for: {filepath} ===")
    
    with open(filepath, 'r', encoding='utf-8') as f:
        for idx, line in enumerate(f, 1):
            # Strip trailing newline
            line_str = line.rstrip()
            
            # Check matches
            m_struct = RE_STRUCT.match(line_str)
            if m_struct:
                print(f"[Line {idx:04d}] Struct: {m_struct.group(1)}")
                continue

            m_enum = RE_ENUM.match(line_str)
            if m_enum:
                print(f"[Line {idx:04d}] Enum: {m_enum.group(1)}")
                continue
                
            m_trait = RE_TRAIT.match(line_str)
            if m_trait:
                print(f"[Line {idx:04d}] Trait: {m_trait.group(1)}")
                continue

            m_impl = RE_IMPL.match(line_str)
            if m_impl:
                trait_name = m_impl.group(1)
                struct_name = m_impl.group(2)
                if trait_name:
                    print(f"[Line {idx:04d}] Implementation: '{trait_name}' for '{struct_name}'")
                else:
                    print(f"[Line {idx:04d}] Implementation block for: '{struct_name}'")
                continue

            m_fn = RE_FN.match(line_str)
            if m_fn:
                print(f"[Line {idx:04d}] Function: {m_fn.group(1)}()")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 rust_parser.py <path_to_rust_file>")
        sys.exit(1)
    
    analyze_rust_file(sys.argv[1])
