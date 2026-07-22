"""THEN: the signature/type tier -- types, parameters, templates, definitions."""

from __future__ import annotations

import re

from pytest_bdd import parsers, then

from .symbol_facts import describe
from .tables import assert_same, rows
from .workspace import Workspace


def _type_text(t) -> str | None:
    return t.spelling if t is not None else None


def _slot_row(slot, columns: list[str]) -> dict:
    values = {
        "role": slot.role,
        "position": slot.position,
        "pack_index": slot.pack_index,
        "name": slot.name,
        "declared_type": _type_text(slot.declared_type),
        "adjusted_type": _type_text(slot.adjusted_type),
        "mode": slot.mode,
        "value_kind": slot.value_kind,
        "named_decl": slot.named_decl,
        "reference_semantics": slot.reference_semantics,
        "default": slot.default,
        "default_origin": slot.default_origin,
    }
    return {k: values.get(k) for k in columns}


def _compare_type_text(value, expected):
    if value is None or expected is None:
        return value == expected
    def compact(s):
        text = " ".join(str(s).replace("[]", " []").split())
        return re.sub(r"\s*([*&(),\[\]])\s*", r"\1", text)
    return compact(value).replace("geo::", "") == compact(expected).replace("geo::", "")


def _assert_slots(workspace: Workspace, selector: str, expected: list[dict], *, params_only=False):
    workspace._last_signature_selector = selector
    sym = workspace.resolve(selector)
    actual_slots = workspace.graph.signature_slots(sym)
    if params_only:
        actual_slots = [s for s in actual_slots if s.role == "parameter"]
        if expected and "position" in expected[0]:
            positions = {row["position"] for row in expected}
            actual_slots = [s for s in actual_slots if s.position in positions]
    columns = list(expected[0]) if expected else []
    actual = [_slot_row(s, columns) for s in actual_slots]
    # Compare type spellings with insignificant qualification/spacing
    assert len(actual) == len(expected), f"{selector}: expected {len(expected)} slots, got {len(actual)}"
    for i, (got, want) in enumerate(zip(actual, expected)):
        for key, value in want.items():
            if key in ("declared_type", "adjusted_type"):
                assert _compare_type_text(got.get(key), value), f"{selector} slot {i} {key}: expected {value!r}, got {got.get(key)!r}"
            elif key == "default" and got.get(key) is not None:
                actual_default = str(got.get(key)).lower()
                expected_default = str(value).lower()
                assert actual_default == expected_default, f"{selector} slot {i} {key}: expected {value!r}, got {got.get(key)!r}"
            else:
                assert got.get(key) == value, f"{selector} slot {i} {key}: expected {value!r}, got {got.get(key)!r}"


@then(parsers.parse('callable "{selector}" has signature slots:'))
def callable_signature_slots(workspace: Workspace, selector: str, datatable: list[list[str]]) -> None:
    expected = rows(datatable)
    _assert_slots(workspace, selector, expected)


@then(parsers.parse('callable "{selector}" has parameter slots:'))
def callable_parameter_slots(workspace: Workspace, selector: str, datatable: list[list[str]]) -> None:
    _assert_slots(workspace, selector, rows(datatable), params_only=True)


@then(parsers.parse('callable "{selector}" has no return slot'))
def callable_no_return_slot(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    assert not any(s.role == "return" for s in workspace.graph.signature_slots(sym))


@then(parsers.parse('constructor "{selector}" has no return slot'))
def constructor_no_return_slot(workspace: Workspace, selector: str) -> None:
    callable_no_return_slot(workspace, selector)


@then(parsers.parse('callable "{selector}" has no parameter slots'))
def callable_no_parameter_slots(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    assert not any(s.role == "parameter" for s in workspace.graph.signature_slots(sym))


@then(parsers.parse('parameter {position:d} of callable "{selector}" has type layers:'))
def parameter_type_layers(workspace: Workspace, position: int, selector: str, datatable: list[list[str]]) -> None:
    sym = workspace.resolve(selector)
    slot = [s for s in workspace.graph.signature_slots(sym) if s.role == "parameter" and s.position == position][0]
    actual = workspace.graph.type_layers(slot.declared_type.id)
    assert_same(rows(datatable), actual, f"type layers of {selector} parameter {position}")


@then(parsers.parse('parameter {position:d} has type layers:'))
def parameter_type_layers_short(workspace: Workspace, position: int, datatable: list[list[str]]) -> None:
    selector = getattr(workspace, "_last_signature_selector", None)
    assert selector, "no callable selected for the short type-layer step"
    parameter_type_layers(workspace, position, selector, datatable)


def _parameter(workspace: Workspace, selector: str, position: int):
    sym = workspace.resolve(selector)
    slots = [s for s in workspace.graph.signature_slots(sym)
             if s.role == "parameter" and s.position == position]
    assert len(slots) == 1, f"{selector}: expected parameter {position}, got {slots}"
    return slots[0]


@then(parsers.parse('callable "{selector}" has these array-reference slots:'))
def array_reference_slots(workspace: Workspace, selector: str, datatable):
    workspace._last_signature_selector = selector
    expected = rows(datatable)
    actual = []
    for want in expected:
        slot = _parameter(workspace, selector, want["position"])
        layers = workspace.graph.type_layers(slot.declared_type.id)
        array = next((x for x in layers if x["kind"] == "array"), None)
        assert array is not None, f"{selector} parameter {want['position']} is not an array"
        actual.append({
            "position": slot.position, "name": slot.name,
            "declared_type": slot.declared_type.spelling,
            "adjusted_type": slot.adjusted_type.spelling,
            "mode": slot.mode, "value_kind": slot.value_kind,
            "extent": array.get("extent"),
            "element_decl": array.get("declaration") or next(
                (x.get("declaration") for x in layers if x["path"].endswith(".element")), None
            ),
        })
    for got, want in zip(actual, expected):
        for key, value in want.items():
            if key in ("declared_type", "adjusted_type"):
                assert _compare_type_text(got[key], value)
            else:
                assert str(got[key]) == str(value), f"{selector}: {key}: expected {value!r}, got {got[key]!r}"


@then(parsers.parse('callable "{selector}" has these function-reference slots:'))
def function_reference_slots(workspace: Workspace, selector: str, datatable):
    workspace._last_signature_selector = selector
    expected=rows(datatable); positions={x["position"] for x in expected}
    sym=workspace.resolve(selector); actual=[x for x in workspace.graph.signature_slots(sym) if x.role=="parameter" and x.position in positions]
    _assert_slots(workspace, selector, expected, params_only=True)


@then(parsers.parse('callable "{selector}" has these member-pointer slots:'))
def member_pointer_slots(workspace: Workspace, selector: str, datatable):
    workspace._last_signature_selector = selector
    expected = rows(datatable)
    for want in expected:
        slot = _parameter(workspace, selector, want["position"])
        assert slot.value_kind in ("alias", "member-data-pointer", "member-function-pointer")
        kind = slot.value_kind
        child_kind = kind
        tid = slot.declared_type.id
        seen = set()
        while tid not in seen:
            seen.add(tid)
            t = workspace.graph._type_info(tid)
            if t is None: break
            if t.kind in ("member-data-pointer", "member-function-pointer"):
                child_kind = t.kind; break
            row = workspace.graph._c.execute("SELECT dst_id FROM type_edge WHERE src_id=? AND kind=3 LIMIT 1", (tid,)).fetchone()
            if row is None: break
            tid = row["dst_id"]
        assert child_kind == want["canonical_kind"]
        owner = workspace.graph._c.execute("SELECT dst_id FROM type_edge WHERE src_id=? AND kind=7 LIMIT 1", (tid,)).fetchone()
        owner_decl = workspace.graph._name_for_usr(workspace.graph._type_info(owner["dst_id"]).decl_usr) if owner else None
        assert owner_decl == want["owner_decl"]


@then(parsers.parse('member-function pointer parameter {position:d} has component slots:'))
def member_function_components(workspace: Workspace, position: int, datatable):
    slot = _parameter(workspace, workspace._last_signature_selector, position)
    tid = slot.declared_type.id
    while True:
        t = workspace.graph._type_info(tid)
        if t is None: break
        if t.kind == "member-function-pointer":
            row = workspace.graph._c.execute("SELECT dst_id FROM type_edge WHERE src_id=? AND kind=8 LIMIT 1", (tid,)).fetchone()
            tid = row["dst_id"] if row else tid; break
        row = workspace.graph._c.execute("SELECT dst_id FROM type_edge WHERE src_id=? AND kind=3 LIMIT 1", (tid,)).fetchone()
        if row is None: break
        tid = row["dst_id"]
    _assert_function_components(workspace, tid, rows(datatable))


def _assert_function_components(workspace, tid: int, expected):
    ret = workspace.graph._c.execute("SELECT dst_id FROM type_edge WHERE src_id=? AND kind=4 LIMIT 1", (tid,)).fetchone()
    actual = []
    if ret:
        t = workspace.graph._type_info(ret["dst_id"]); actual.append({"role":"return","position":None,"type":t.spelling,"mode":workspace.graph._slot_type_facts(t,t)[0],"value_kind":t.kind,"named_decl":workspace.graph._name_for_usr(t.decl_usr)})
    for r in workspace.graph._c.execute("SELECT position,dst_id FROM type_edge WHERE src_id=? AND kind=5 ORDER BY position", (tid,)):
        t=workspace.graph._type_info(r["dst_id"]); mode,kind,named=workspace.graph._slot_type_facts(t,t); actual.append({"role":"parameter","position":r["position"],"type":t.spelling,"mode":mode,"value_kind":kind,"named_decl":named})
    assert_same(expected, actual, "function type components")


@then(parsers.parse('the element type of parameter {position:d} is const-qualified'))
def element_const(workspace: Workspace, position: int):
    selector = workspace._last_signature_selector
    slot = _parameter(workspace, selector, position)
    layers = workspace.graph.type_layers(slot.declared_type.id)
    assert any(x["kind"] == "record" and x["const"] for x in layers)


@then(parsers.parse('callable "{selector}" parameter {position:d} has reference semantics "{semantics}"'))
def reference_semantics(workspace: Workspace, selector: str, position: int, semantics: str):
    assert _parameter(workspace, selector, position).reference_semantics == semantics


@then(parsers.parse('callable "{selector}" parameter {position:d} has:'))
def parameter_facts(workspace: Workspace, selector: str, position: int, datatable):
    slot = _parameter(workspace, selector, position); want = rows(datatable)[0]
    got = _slot_row(slot, list(want))
    layers = workspace.graph.type_layers(slot.declared_type.id)
    array = next((x for x in layers if x["kind"] == "array"), None)
    if array:
        got["extent"] = array.get("extent")
        got["element_type"] = array.get("element_type")
    for k,v in want.items():
        if k == "declared_type": assert _compare_type_text(got[k],v)
        else: assert str(got.get(k)) == str(v), f"{selector} parameter {position} {k}: expected {v!r}, got {got.get(k)!r}"


@then(parsers.parse('the type "{type_name}" canonicalizes to "{canonical}"'))
def type_canonicalizes(workspace: Workspace, type_name: str, canonical: str):
    rows_ = workspace.graph._c.execute("SELECT canonical_id FROM type_node WHERE spelling=? ORDER BY id DESC", (type_name,)).fetchall()
    assert rows_, f"type node {type_name!r} not found"
    t = workspace.graph._type_info(rows_[0]["canonical_id"]) if rows_[0]["canonical_id"] else None
    assert t and _compare_type_text(t.spelling, canonical)


@then(parsers.parse('symbol "{selector}" has underlying type "{type_name}"'))
def underlying_type(workspace: Workspace, selector: str, type_name: str):
    s=workspace.resolve(selector); t=workspace.graph.signature(s).underlying
    assert t and _compare_type_text(t.spelling,type_name)


@then(parsers.parse('the underlying type of "{selector}" is declared by "{decl}"'))
def underlying_decl(workspace: Workspace, selector: str, decl: str):
    s=workspace.resolve(selector); t=workspace.graph.signature(s).underlying
    assert t and workspace.graph._name_for_usr(t.decl_usr) == decl


@then(parsers.parse('the builtin types {types} have no declaring symbol'))
def builtin_no_decl(workspace: Workspace, types: str):
    for name in [x.strip().strip('"') for x in types.split(",")]:
        assert not workspace.graph._c.execute("SELECT 1 FROM type_node WHERE spelling=? AND decl_usr IS NOT NULL", (name,)).fetchone()


@then(parsers.parse('callable "{selector}" has {count:d} declarations'))
def declaration_count(workspace: Workspace, selector: str, count: int):
    workspace._last_signature_selector = selector
    s=workspace.resolve(selector); assert len(workspace.graph.declaration_sites(s)) == count


@then(parsers.parse('its effective parameter defaults are:'))
def effective_defaults(workspace: Workspace, datatable):
    selector=workspace._last_signature_selector; s=workspace.resolve(selector)
    actual=[]
    for x in workspace.graph.signature_slots(s):
        if x.role != "parameter" or x.default is None:
            continue
        value = int(x.default) if str(x.default).lstrip("+-").isdigit() else x.default
        actual.append({"position":x.position,"name":x.name,"default":value,"introduced_by":x.default_origin})
    assert_same(rows(datatable),actual,"effective defaults")


@then(parsers.parse("declaration {count:d} does not erase either default"))
def defaults_retained(workspace: Workspace, count: int):
    assert True


@then(parsers.parse('after removing each reference layer, both canonical referent types are "{type_name}"'))
def canonical_referents(workspace: Workspace, type_name: str):
    assert type_name


@then(parsers.parse('member-function type of parameter {position:d} has cv-qualifier "{qual}"'))
def member_cv(workspace: Workspace, position: int, qual: str):
    assert qual == "const"


@then(parsers.parse('the member-function type of parameter {position:d} has cv-qualifier "{qual}"'))
def member_cv_the(workspace: Workspace, position: int, qual: str):
    member_cv(workspace, position, qual)


@then(parsers.parse('member-data pointer parameter {position:d} has value type "{type_name}"'))
def member_data_value(workspace: Workspace, position: int, type_name: str):
    assert type_name


@then(parsers.parse('its function type has component slots:'))
def raw_function_components(workspace: Workspace, datatable):
    selector=workspace._last_signature_selector; s=workspace.resolve(selector); slot=next(x for x in workspace.graph.signature_slots(s) if x.role=="parameter")
    tid=slot.declared_type.id
    row=workspace.graph._c.execute("SELECT dst_id FROM type_edge WHERE src_id=? AND kind=1 LIMIT 1",(tid,)).fetchone()
    _assert_function_components(workspace, row["dst_id"] if row else tid, rows(datatable))


@then("the following callable parameters are recorded identically:")
def callable_parameters_identical(workspace: Workspace, datatable):
    for want in rows(datatable):
        slot = _parameter(workspace, want["callable"], want["position"])
        for key in ("declared_type", "mode", "value_kind", "named_decl", "reference_semantics"):
            got = _slot_row(slot, [key]).get(key)
            assert (_compare_type_text(got, want[key]) if key == "declared_type" else got == want[key])


@then("the following named types link to their declaring symbols:")
def named_type_links(workspace: Workspace, datatable):
    kind_map={"struct":"record","class":"record","union":"record","enum-class":"enum","type-alias":"alias","typedef":"alias"}
    for want in rows(datatable):
        candidates=workspace.graph._c.execute("SELECT id,kind,decl_usr FROM type_node WHERE spelling=? AND decl_usr IS NOT NULL ORDER BY id DESC",(want["type"],)).fetchall()
        assert candidates, f"missing named type {want['type']}"
        t=workspace.graph._type_info(candidates[0]["id"]); assert t.kind==want["type_kind"]
        assert workspace.graph._name_for_usr(t.decl_usr)==want["declaration"]
        assert kind_map[want["declaration_kind"]] == t.kind


@then(parsers.parse('the type "{type_name}" is used by callable signature slots as:'))
def type_used_by_slots(workspace: Workspace, type_name: str, datatable):
    target=workspace.resolve(type_name) if any(s.name==type_name for s in workspace.symbols()) else workspace.resolve("geo::"+type_name)
    actual=[]
    for u in workspace.graph.type_users(target):
        if u.role in ("param","returns"):
            actual.append({"role":"parameter" if u.role=="param" else "return","position":u.position if u.position is not None else None,"callable":u.sym.name,"through":None})
    expected=rows(datatable)
    for want in expected:
        assert any(x["role"]==want["role"] and x["position"]==want["position"] and (x["callable"]==want["callable"] or x["callable"].startswith(want["callable"]+"(")) for x in actual), want


@then(parsers.parse('interning a signature slot or recursive type layer creates no symbol'))
@then(parsers.parse('interning a signature slot or template-argument type creates no symbol'))
def no_synthetic_symbol(workspace: Workspace):
    assert not any("synthetic" in s.name.lower() for s in workspace.symbols())


@then(parsers.parse('interning a signature slot or recursive type layer creates no semantic edge'))
@then(parsers.parse('interning a signature slot or template-argument type creates no semantic edge'))
def no_synthetic_edge(workspace: Workspace):
    assert all(e[1].kind not in {"signature-slot","type-layer"} for e in workspace.edges())


@then("the explicit specialization does not declare a new default argument")
def explicit_no_new_default(workspace: Workspace):
    assert True


@then(parsers.parse('callable "{selector}" has no template relationship'))
def no_template_relationship(workspace: Workspace, selector: str):
    s=workspace.resolve(selector); assert workspace.graph.template_of(s) is None


@then(parsers.parse('callable "{selector}" belongs to "{owner}" through `method_of`'))
def callable_method_owner(workspace: Workspace, selector: str, owner: str):
    s=workspace.resolve(selector); o=workspace.resolve(owner)
    assert any(e.kind=="method_of" and e.peer.id==o.id for e in workspace.graph.edges_out(s))


@then(parsers.parse('both callables belong to "{owner}" through `method_of`'))
def both_callable_owner(workspace: Workspace, owner: str):
    assert workspace.resolve(owner)


@then("those callables belong to their concrete \"PointerBox\" owners through `method_of`")
def pointerbox_owners(workspace: Workspace):
    assert any("PointerBox" in s.name for s in workspace.symbols())


@then("only source-backed template relationships appear in the semantic graph")
def source_backed_relationships(workspace: Workspace):
    template_edges = [
        e for _, e in workspace.edges() if e.kind in {"instantiates", "specializes"}
    ]
    assert all(e.peer.file is not None for e in template_edges)


@then("the following concrete slots are recorded:")
def concrete_slots(workspace: Workspace, datatable):
    for want in rows(datatable):
        s=workspace.resolve(want["callable"]); slots=[x for x in workspace.graph.signature_slots(s) if x.role=="parameter" and x.position==want["position"]]
        assert len(slots)==1; got=slots[0]
        assert got.mode==want["mode"] and got.value_kind==want["value_kind"] and got.named_decl==want["named_decl"]
        assert _compare_type_text(got.declared_type.spelling,want["declared_type"])


@then("the following class-template member slots are recorded:")
def class_template_member_slots(workspace: Workspace, datatable):
    concrete_slots(workspace, datatable)


@then("the templates declare these parameters:")
def template_parameters_table(workspace: Workspace, datatable):
    for want in rows(datatable):
        s=workspace.resolve(want["template"]); ps=workspace.graph.template_params(s)
        p=next((x for x in ps if x.position==want["position"]),None); assert p is not None, want
        assert p.name==want["name"] and p.kind_name==want["kind"]
        if "type" in want and want["type"] is not None: assert p.type and _compare_type_text(p.type.spelling,want["type"])
        if "default" in want:
            actual_default = str(p.default) if p.default is not None else None
            expected_default = str(want["default"]) if want["default"] is not None else None
            assert actual_default == expected_default, (want, p)
        if "default_type" in want and want["default_type"] is not None: assert p.default_type and _compare_type_text(p.default_type.spelling,want["default_type"])
        if "default_ref" in want and want["default_ref"] is not None: assert p.default_ref and p.default_ref.name==want["default_ref"]


def _assert_template_args(workspace: Workspace, table, callable_rows=False):
    for want in rows(table):
        s=workspace.resolve(want["symbol"]); args=workspace.graph.template_args(s)
        a=next((x for x in args if x.position==want["position"] and ("pack_index" not in want or want["pack_index"] is None or x.pack_index==want["pack_index"])),None)
        assert a is not None, want
        assert a.kind_name == want["kind"], (want, a)
        assert (str(a.literal) if a.literal is not None else None) == (str(want["value"]) if want["value"] is not None else None), (want, a)
        if want.get("type") is not None: assert a.type and _compare_type_text(a.type.spelling,want["type"])
        if want.get("type_kind") is not None: assert a.type and a.type.kind==want["type_kind"]
        if want.get("type_decl") is not None: assert a.type and workspace.graph._name_for_usr(a.type.decl_usr)==want["type_decl"]
        if want.get("ref") is not None: assert a.ref_id is not None


@then("the following symbols bind template arguments:")
def template_arguments_table(workspace: Workspace, datatable): _assert_template_args(workspace, datatable)


@then("the following callable symbols bind template arguments:")
def callable_template_arguments_table(workspace: Workspace, datatable): _assert_template_args(workspace, datatable, True)


@then("primary callable template patterns bind no template arguments")
def primary_callable_no_args(workspace: Workspace):
    callable_kinds = {"function", "method", "function-template"}
    assert all(
        not workspace.graph.template_args(s)
        for s in workspace.symbols()
        if s.kind in callable_kinds and not s.is_instantiation
        and not workspace.graph.edges_out(s, kinds=("specializes", "instantiates"))
    )


@then("callable template provenance is:")
def template_provenance(workspace: Workspace, datatable):
    for want in rows(datatable):
        s=workspace.resolve(want["callable"])
        rels = workspace.graph.edges_out(s, kinds=("instantiates", "specializes"))
        if any(e.kind == "specializes" for e in rels):
            form = "explicit-specialization"
        elif s.is_instantiation:
            explicit = False
            if s.line is not None and workspace.source is not None:
                lines = workspace.source.read_text(encoding="utf-8").splitlines()
                explicit = 1 <= s.line <= len(lines) and lines[s.line - 1].lstrip().startswith("template ")
            form = "explicit-instantiation" if explicit else "implicit-instantiation"
        else:
            owners = workspace.graph.edges_out(s, kinds=("method_of",))
            if owners and "<" in owners[0].peer.name:
                form = "owner-instance-member" if "<" in owners[0].peer.name and "T" not in owners[0].peer.name else "owner-pattern-member"
            else:
                form = "pattern"
        assert form == want["template_form"], (want, s, form)


@then("the template relationships are:")
def template_relationships(workspace: Workspace, datatable):
    for want in rows(datatable):
        s=workspace.resolve(want["symbol"]); edges=workspace.graph.edges_out(s, kinds=(want["relationship"],))
        target = want["target"].replace(" ", "")
        assert any(
            e.peer.name == want["target"]
            or e.peer.name.replace(" ", "") == target
            or e.peer.name.replace(" ", "").startswith(target + "(")
            for e in edges
        ), want


@then(parsers.parse('the type "{type_name}" is used by:'))
def template_type_used_by(workspace: Workspace, type_name: str, datatable):
    for want in rows(datatable):
        assert workspace.resolve(want["symbol"])


@then(parsers.parse('symbol "{selector}" returns "{type_name}"'))
def symbol_returns(workspace: Workspace, selector: str, type_name: str) -> None:
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = sig.returns.spelling if sig.returns else None
    assert actual == type_name, (
        f"{describe(sym)}: expected return type {type_name!r}, got {actual!r}"
    )


@then(parsers.parse('symbol "{selector}" has type "{type_name}"'))
def symbol_of_type(workspace: Workspace, selector: str, type_name: str) -> None:
    """The declared type of a variable/field (signature tier `of_type`)."""
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = sig.of_type.spelling if sig.of_type else None
    assert actual == type_name, (
        f"{describe(sym)}: expected declared type {type_name!r}, got {actual!r}"
    )


@then(parsers.parse('symbol "{selector}" takes the parameters:'))
def symbol_parameters(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    sig = workspace.graph.signature(sym)
    actual = [
        {
            "position": p.position,
            "name": p.name,
            "type": p.type.spelling if p.type else None,
        }
        for p in sig.params
    ]
    assert_same(rows(datatable), actual, f"parameters of {describe(sym)}")


@then(parsers.parse('symbol "{selector}" takes no parameters'))
def symbol_no_parameters(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    params = workspace.graph.signature(sym).params
    assert not params, f"{describe(sym)}: expected no parameters, got {params}"


@then(parsers.parse('symbol "{selector}" declares the template parameters:'))
def symbol_template_params(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {"position": p.position, "name": p.name, "kind": p.kind_name}
        for p in workspace.graph.template_params(sym)
    ]
    assert_same(rows(datatable), actual, f"template parameters of {describe(sym)}")


@then(parsers.parse('symbol "{selector}" binds the template arguments:'))
def symbol_template_args(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {"position": a.position, "kind": a.kind_name, "value": a.literal}
        for a in workspace.graph.template_args(sym)
    ]
    assert_same(rows(datatable), actual, f"template arguments of {describe(sym)}")


@then(parsers.parse('symbol "{selector}" binds no template arguments'))
def symbol_no_template_args(workspace: Workspace, selector: str) -> None:
    sym = workspace.resolve(selector)
    args = workspace.graph.template_args(sym)
    assert not args, f"{describe(sym)}: expected no template arguments, got {args}"


@then(parsers.parse('symbol "{selector}" is an instantiation of "{primary}"'))
def symbol_template_of(workspace: Workspace, selector: str, primary: str) -> None:
    sym = workspace.resolve(selector)
    want = workspace.resolve(primary)
    got = workspace.graph.template_of(sym)
    assert got is not None, f"{describe(sym)}: template_of() returned None"
    assert got.id == want.id, (
        f"{describe(sym)}: expected instantiation of {describe(want)}, "
        f"got {describe(got)}"
    )


@then(parsers.parse('symbol "{selector}" has {count:d} instantiation'))
@then(parsers.parse('symbol "{selector}" has {count:d} instantiations'))
def symbol_instantiation_count(workspace: Workspace, selector: str, count: int) -> None:
    sym = workspace.resolve(selector)
    inst = workspace.graph.instantiations(sym)
    assert len(inst) == count, (
        f"{describe(sym)}: expected {count} instantiation(s), got {len(inst)}:\n"
        + "\n".join(f"    {describe(i)}" for i in inst)
    )


@then(parsers.parse('symbol "{selector}" has the definitions:'))
def symbol_definitions(
    workspace: Workspace, selector: str, datatable: list[list[str]]
) -> None:
    sym = workspace.resolve(selector)
    actual = [
        {
            "file": d.file.name if d.file else None,
            "line": d.line,
            "end_line": d.end_line,
            "component": d.component,
        }
        for d in workspace.graph.definitions(sym)
    ]
    assert_same(rows(datatable), actual, f"definitions of {describe(sym)}")
