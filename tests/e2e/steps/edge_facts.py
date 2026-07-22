"""Rendering and lookup helpers for edges and their sites."""

from __future__ import annotations

from pathlib import Path

from .workspace import Workspace


def edge_repr(src, edge) -> str:
    sites = ",".join(f"{s.line}:{s.col}" for s in edge.sites)
    return (
        f"{src.name}#{src.id} --{edge.kind}(count={edge.count})--> "
        f"{edge.peer.name}#{edge.peer.id}" + (f"  @[{sites}]" if sites else "")
    )


def site_repr(src, edge, site) -> str:
    return f"{src.name} --{edge.kind}--> {edge.peer.name}  @ {site.loc}"


def all_sites(ws: Workspace) -> list[tuple]:
    """Every (src Sym, Edge, Site) triple in the index, edge order preserved."""
    out: list[tuple] = []
    for src, edge in ws.edges():
        for site in edge.sites:
            out.append((src, edge, site))
    return out


def source_line(ws: Workspace, site) -> str:
    """The fixture's raw source line a site points at (1-based site.line)."""
    path = Path(site.file) if site.file else None
    if path is None or not path.is_file():
        path = ws.source
    assert path is not None and path.is_file(), (
        f"cannot read the source for site {site.loc}: no such file {path}"
    )
    lines = path.read_text(encoding="utf-8").splitlines()
    assert site.line is not None and 1 <= site.line <= len(lines), (
        f"site {site.loc} points at line {site.line}, but {path.name} has "
        f"{len(lines)} line(s)"
    )
    return lines[site.line - 1]
