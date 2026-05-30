#!/usr/bin/env python3
import sys
import json
import base64
import subprocess
import urllib.request
import urllib.parse
from urllib.error import URLError, HTTPError

def get_neo4j_credentials():
    try:
        # Run kubectl to get the secret from admin@hs-cluster
        cmd = [
            "/opt/homebrew/bin/kubectl",
            "--context", "admin@hs-cluster",
            "get", "secret", "neo4j-auth-external",
            "-n", "infrastructure",
            "-o", "jsonpath={.data.NEO4J_AUTH}"
        ]
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        encoded_auth = res.stdout.strip()
        decoded_auth = base64.b64decode(encoded_auth).decode('utf-8')
        
        # Split into user and password
        user, password = decoded_auth.split('/', 1)
        return user, password
    except Exception as e:
        print(f"Error fetching Neo4j credentials from kubernetes: {e}", file=sys.stderr)
        sys.exit(1)

def run_cypher_query(query):
    user, password = get_neo4j_credentials()
    
    url = "https://neo4j.senussi.me/db/neo4j/tx/commit"
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json;charset=UTF-8",
        "Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode('utf-8')).decode('utf-8')
    }
    
    data = {
        "statements": [
            {
                "statement": query
            }
        ]
    }
    
    req_data = json.dumps(data).encode('utf-8')
    req = urllib.request.Request(url, data=req_data, headers=headers, method="POST")
    
    try:
        # Ignore SSL verification issues since it's an internal wildcard/senussi.me domain
        import ssl
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        
        with urllib.request.urlopen(req, context=ctx) as response:
            res_body = response.read().decode('utf-8')
            res_json = json.loads(res_body)
            
            # Print errors if any
            if "errors" in res_json and res_json["errors"]:
                print("Neo4j errors returned:", file=sys.stderr)
                for err in res_json["errors"]:
                    print(f"[{err.get('code')}] {err.get('message')}", file=sys.stderr)
                sys.exit(1)
                
            # Print results format
            results = res_json.get("results", [])
            for res in results:
                columns = res.get("columns", [])
                print(" | ".join(columns))
                print("-" * (len(columns) * 15))
                for row in res.get("data", []):
                    row_vals = [str(val) for val in row.get("row", [])]
                    print(" | ".join(row_vals))
                    
    except HTTPError as e:
        print(f"HTTP Error: {e.code} - {e.reason}", file=sys.stderr)
        try:
            error_details = e.read().decode('utf-8')
            print(f"Details: {error_details}", file=sys.stderr)
        except Exception:
            pass
        sys.exit(1)
    except URLError as e:
        print(f"URL Error: {e.reason}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 query_graph.py <cypher_query>")
        print("Example: python3 query_graph.py \"MATCH (n:Node) RETURN count(n) AS node_count\"")
        sys.exit(1)
        
    run_cypher_query(sys.argv[1])
