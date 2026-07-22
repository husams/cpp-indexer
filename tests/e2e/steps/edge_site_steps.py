"""THEN: edge *sites* -- where in the source a relationship is written."""

from __future__ import annotations

import os

from pytest_bdd import parsers, then

from .edge_facts import all_sites, site_repr, source_line
from .tables import rows
from .workspace import Workspace


@then(parsers.parse('the "{kind}" edge sites are:'))
def edge_sites_of_kind(
    workspace: Workspace, kind: str, datatable: list[list[str]]
) -> None:
    """Every SITE of one edge kind -- one row per site, exhaustive for that kind.

    Table columns: src | dst | file | line | col | [token]

    A site is the concrete `file:line:col` where a relationship is *written*,
    which is not the declaration line of either endpoint. `token` is the source
    text the site must point at: it is read back from the fixture on disk, so
    the stated line and column are grounded in real text instead of merely
    echoing the database.
    """
    actual = [t for t in all_sites(workspace) if t[1].kind == kind]
    rendered = [site_repr(*triple) for triple in actual]
    matched: set[int] = set()

    for row in rows(datatable):
        unknown = set(row) - {"src", "dst", "file", "line", "col", "token"}
        assert not unknown, f"unknown edge site column(s) {sorted(unknown)}"
        for required in ("src", "dst", "line", "col"):
            assert row.get(required) is not None, (
                f"edge site rows require a {required!r} cell; got {row}"
            )
        src = workspace.resolve(row["src"])
        dst = workspace.resolve(row["dst"])
        hits = [
            i
            for i, (s, e, site) in enumerate(actual)
            if s.id == src.id
            and e.peer.id == dst.id
            and site.line == row["line"]
            and site.col == row["col"]
        ]
        assert len(hits) == 1, (
            f"expected exactly 1 {kind!r} site "
            f"{row['src']} -> {row['dst']} at {row['line']}:{row['col']}, "
            f"found {len(hits)}.\n  all {kind!r} sites:\n"
            + "\n".join(f"    {r}" for r in rendered)
        )
        index = hits[0]
        assert index not in matched, (
            "the edge site table mentions the same site more than once: "
            f"{rendered[index]}"
        )
        _, _, site = actual[index]

        if row.get("file") is not None:
            got_file = os.path.basename(site.file) if site.file else None
            assert got_file == row["file"], (
                f"site {rendered[index]}: expected file {row['file']!r}, "
                f"got {got_file!r}"
            )
        if row.get("token") is not None:
            token = str(row["token"])
            text = source_line(workspace, site)
            start = site.col - 1
            got = text[start : start + len(token)]
            assert got == token, (
                f"site {rendered[index]}: expected column {site.col} to start "
                f"the token {token!r}, got {got!r}\n"
                f"    {os.path.basename(site.file or '')}:{site.line}\n"
                f"    {text}\n"
                f"    {' ' * start}^ col {site.col}"
            )
        matched.add(index)

    extra = [r for i, r in enumerate(rendered) if i not in matched]
    assert not extra, (
        f"index holds {kind!r} edge sites the table does not mention:\n"
        + "\n".join(f"    {r}" for r in extra)
    )
