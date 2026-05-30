#!/usr/bin/env python3
import sys
import os
import json
import subprocess

def find_compile_commands(start_path):
    # Walk up to find compile_commands.json
    curr = os.path.abspath(start_path)
    if os.path.isfile(curr):
        curr = os.path.dirname(curr)
    
    while curr != os.path.dirname(curr):
        cc_path = os.path.join(curr, "compile_commands.json")
        if os.path.exists(cc_path):
            return cc_path
        # Probe common build dirs
        for build_dir in ["build", "out", "cmake-build-debug", "cmake-build-release"]:
            cc_path = os.path.join(curr, build_dir, "compile_commands.json")
            if os.path.exists(cc_path):
                return cc_path
        curr = os.path.dirname(curr)
    return None

def extract_flags(cc_path, target_file):
    if not cc_path:
        return []
    
    target_abs = os.path.abspath(target_file)
    try:
        with open(cc_path, 'r', encoding='utf-8') as f:
            entries = json.load(f)
        
        for entry in entries:
            file_path = entry.get("file", "")
            file_abs = os.path.abspath(os.path.join(os.path.dirname(cc_path), file_path) if not os.path.isabs(file_path) else file_path)
            if file_abs == target_abs:
                # Extract arguments or command
                args = entry.get("arguments", [])
                if not args and "command" in entry:
                    args = entry["command"].split()
                
                # Filter out compiler driver, source file, output file flag pairs
                filtered_flags = []
                skip_next = False
                for i, arg in enumerate(args):
                    if skip_next:
                        skip_next = False
                        continue
                    if i == 0:  # Skip driver token
                        continue
                    if arg in ("-c", "-o"):
                        skip_next = True
                        continue
                    # Skip source file repeat
                    if os.path.abspath(arg) == target_abs:
                        continue
                    filtered_flags.append(arg)
                return filtered_flags
    except Exception as e:
        print(f"Warning: Failed to parse compile_commands.json: {e}", file=sys.stderr)
    return []

def print_ast_node(node, target_basename, indent=0):
    kind = node.get("kind", "")
    name = node.get("name", "")
    
    decl_kinds = {
        "NamespaceDecl": "Namespace",
        "CXXRecordDecl": "Class/Struct",
        "FunctionDecl": "Function",
        "CXXMethodDecl": "Method",
        "FieldDecl": "Field",
        "VarDecl": "Variable",
        "TypedefDecl": "Typedef",
        "EnumDecl": "Enum",
        "EnumConstantDecl": "EnumConstant",
        "ClassTemplateDecl": "ClassTemplate",
        "FunctionTemplateDecl": "FunctionTemplate"
    }
    
    # Check if the node is defined in our target file (exclude system headers or imports)
    loc = node.get("loc", {})
    # Simple check: if expansionLoc exists, use it
    loc = node.get("range", {}).get("begin", {}).get("expansionLoc", loc)
    
    is_decl = kind in decl_kinds
    if is_decl:
        if not loc.get("file") or name.startswith("__") or name in ("operator new", "operator new[]", "operator delete", "operator delete[]"):
            pass
        else:
            line = loc.get("line", "")
            line_str = f" [Line {line}]" if line else ""
            type_str = ""
            if "type" in node:
                type_str = f" : {node['type'].get('qualType', '')}"
                
            print("  " * indent + f"- {decl_kinds[kind]}: {name}{type_str}{line_str}")
            indent += 1
        
        
    for child in node.get("inner", []):
        child_loc = child.get("loc", {})
        child_loc = child.get("range", {}).get("begin", {}).get("expansionLoc", child_loc)
        child_file = child_loc.get("file")
        # If it has a file name and it's not our file, skip
        if child_file and os.path.basename(child_file) != target_basename:
            continue
        print_ast_node(child, target_basename, indent)

def analyze_ast(target_file):
    if not os.path.exists(target_file):
        print(f"Error: File '{target_file}' not found.", file=sys.stderr)
        sys.exit(1)
        
    cc_path = find_compile_commands(target_file)
    flags = extract_flags(cc_path, target_file)
    
    # Call clang to dump the AST in JSON
    cmd = ["clang", "-Xclang", "-ast-dump=json", "-fsyntax-only"] + flags + [target_file]
    
    try:
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0 and not res.stdout:
            print(f"Clang failed to parse file. Error:\n{res.stderr}", file=sys.stderr)
            sys.exit(1)
            
        ast_json = json.loads(res.stdout)
        print(f"=== AST Semantic analysis for: {target_file} ===")
        print_ast_node(ast_json, os.path.basename(target_file))
    except Exception as e:
        print(f"Error running clang or parsing JSON: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 ast_analyzer.py <path_to_cpp_file>")
        sys.exit(1)
    analyze_ast(sys.argv[1])
