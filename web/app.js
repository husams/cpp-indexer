const graphState = (() => {
  const completenessRank = {unknown: 0, partial: 1, complete: 2};
  const freshnessRank = {stale: 0, unknown: 1, unverifiable: 1, current: 2};
  const weakest = (left, right, ranks) => {
    if (typeof left !== 'string') return right;
    if (typeof right !== 'string') return left;
    return (ranks[left] ?? 1) <= (ranks[right] ?? 1) ? left : right;
  };
  const mergeFlags = (left = {}, right = {}) => {
    const merged = {...left, ...right};
    new Set([...Object.keys(left), ...Object.keys(right)]).forEach((key) => {
      if (typeof left[key] === 'boolean' || typeof right[key] === 'boolean') merged[key] = Boolean(left[key] || right[key]);
    });
    return merged;
  };
  const mergeStatus = (left = {}, right = {}) => {
    const merged = mergeFlags(left, right);
    if (left.completeness || right.completeness) merged.completeness = weakest(left.completeness, right.completeness, completenessRank);
    if (left.freshness || right.freshness) merged.freshness = weakest(left.freshness, right.freshness, freshnessRank);
    return merged;
  };
  const mergeSites = (left = [], right = []) => {
    const seen = new Set();
    return [...left, ...right].filter((site) => {
      const key = JSON.stringify(site);
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    });
  };
  const mergeRecord = (left, right) => {
    const merged = {...left, ...right};
    if (left.status || right.status) merged.status = mergeStatus(left.status, right.status);
    if (left.evidence || right.evidence) merged.evidence = mergeFlags(left.evidence, right.evidence);
    if (Array.isArray(left.sites) || Array.isArray(right.sites)) merged.sites = mergeSites(left.sites, right.sites);
    return merged;
  };
  const mergeMetadata = (left = {}, right = {}, sitesUsed) => {
    const merged = {...left, ...right};
    ['truncated', 'evidence_truncated'].forEach((key) => { merged[key] = Boolean(left[key] || right[key]); });
    merged.continuation = {...left.continuation, ...right.continuation};
    merged.continuation.available = Boolean(left.continuation?.available || right.continuation?.available);
    if (left.continuation?.reason === 'byte_budget' || right.continuation?.reason === 'byte_budget') merged.continuation.reason = 'byte_budget';
    if (sitesUsed !== undefined) merged.sites_used = sitesUsed;
    return merged;
  };
  const mergeSlices = (left, right) => {
    const nodes = new Map((left.nodes || []).map((node) => [String(node.id), node]));
    (right.nodes || []).forEach((node) => {
      const id = String(node.id);
      nodes.set(id, nodes.has(id) ? mergeRecord(nodes.get(id), node) : node);
    });
    const edges = new Map((left.edges || []).map((edge) => [String(edge.id), edge]));
    (right.edges || []).forEach((edge) => {
      const id = String(edge.id);
      edges.set(id, edges.has(id) ? mergeRecord(edges.get(id), edge) : edge);
    });
    const sitesUsed = [...edges.values()].reduce((total, edge) => total + (edge.sites || []).length, 0);
    const mergedNodes = [...nodes.values()];
    const mergedEdges = [...edges.values()];
    const elementTruncated = [...mergedNodes, ...mergedEdges].some((element) => element.status?.truncated === true);
    const evidenceTruncated = mergedEdges.some((edge) => edge.status?.evidence_truncated === true || edge.evidence?.sites_truncated === true);
    const metadata = mergeMetadata(left.metadata, right.metadata, sitesUsed);
    metadata.truncated = Boolean(metadata.truncated || elementTruncated);
    metadata.evidence_truncated = Boolean(metadata.evidence_truncated || evidenceTruncated);
    metadata.continuation.available = Boolean(metadata.continuation.available || metadata.truncated);
    return {...left, metadata, nodes: mergedNodes, edges: mergedEdges};
  };
  return {mergeSlices};
})();

if (typeof module !== 'undefined') module.exports = graphState;
if (typeof document !== 'undefined') {
(() => {
  const embedded = window.CIDX_GRAPH_VIEW || {nodes: [], edges: [], metadata: {}};
  const liveToken = new URLSearchParams(window.location.search).get('token');
  let view = embedded;
  let cy;
  let selectedNode;
  const nodeById = new Map();
  const edgeIds = new Set();
  const details = document.getElementById('details');
  const title = document.getElementById('selection-title');
  const expandButton = document.getElementById('expand');
  const accessible = document.getElementById('accessible-nodes');
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const badge = (name, cls) => `<span class="badge ${cls || ''}">${esc(name)}</span>`;
  const statusBadges = (s = {}) => Object.entries(s)
    .filter(([k, v]) => v === true || v === 'partial' || v === 'stale' || v === 'truncated' || v === 'external')
    .map(([k, v]) => badge(`${k}${v === true ? '' : `: ${v}`}`, k === 'external' ? 'external' : k))
    .join('');
  const sourceLocation = (item) => item?.location || (item?.file ? `${item.file}:${item.line ?? ''}:${item.col ?? ''}` : 'No location');
  const updateSummary = () => {
    document.getElementById('summary').textContent = `${view.nodes?.length || 0} nodes · ${view.edges?.length || 0} edges${view.metadata?.truncated ? ' · bounded/truncated' : ''}`;
    document.getElementById('identity').textContent = view.metadata?.index?.freshness ? `index ${view.metadata.index.freshness}` : 'index freshness unverifiable';
  };
  const showNode = (n) => {
    selectedNode = n;
    expandButton.disabled = !liveToken;
    title.textContent = n.name || n.usr || 'Node';
    const evidence = n.evidence?.location || 'No bounded evidence attached';
    details.innerHTML = `<dl><dt>Identity</dt><dd><code>${esc(n.usr || n.id)}</code></dd><dt>Kind</dt><dd>${esc(n.kind)}</dd><dt>Location</dt><dd>${esc(sourceLocation(n))}</dd><dt>Status</dt><dd>${statusBadges(n.status) || 'No status flags'}</dd><dt>Evidence</dt><dd>${esc(evidence)}</dd></dl>`;
  };
  const showEdge = (e) => {
    selectedNode = undefined;
    expandButton.disabled = true;
    title.textContent = `${e.kind} relation`;
    details.innerHTML = `<dl><dt>From</dt><dd>${esc(nodeById.get(String(e.source))?.name || e.source)}</dd><dt>To</dt><dd>${esc(nodeById.get(String(e.target))?.name || e.target)}</dd><dt>Count</dt><dd>${esc(e.count)}</dd><dt>Status</dt><dd>${statusBadges(e.status) || 'No status flags'}</dd><dt>Evidence</dt><dd>${(e.sites || []).map((s) => esc(sourceLocation(s))).join('<br>') || 'No site evidence'}</dd></dl>`;
  };
  const elementForNode = (n) => ({data: {...n, id: String(n.id), label: n.name || n.usr}});
  const elementForEdge = (e) => ({data: {...e, id: String(e.id), source: String(e.source), target: String(e.target)}});
  const rebuildAccessibleNodes = () => {
    accessible.replaceChildren();
    nodeById.forEach((n) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = `${n.name || n.usr} (${n.kind})`;
      button.setAttribute('aria-label', `${n.name || n.usr}, ${n.status?.completeness || 'unknown'} status`);
      button.onclick = () => {
        const node = cy.$id(String(n.id));
        node.select();
        cy.animate({center: {eles: node}, duration: 150});
        showNode(n);
      };
      accessible.appendChild(button);
    });
  };
  const addView = (next, append) => {
    if (append) {
      const merged = graphState.mergeSlices(view, next);
      const newElements = [];
      merged.nodes.forEach((n) => {
        const id = String(n.id);
        nodeById.set(id, n);
        if (!cy.$id(id).length) newElements.push(elementForNode(n));
        else cy.$id(id).data({...n, id, label: n.name || n.usr});
      });
      merged.edges.forEach((e) => {
        const id = String(e.id);
        if (!edgeIds.has(id)) newElements.push(elementForEdge(e));
        else cy.$id(id).data({...e, id, source: String(e.source), target: String(e.target)});
        edgeIds.add(id);
      });
      cy.add(newElements);
      view = merged;
    } else {
      view = next;
      nodeById.clear();
      edgeIds.clear();
      (view.nodes || []).forEach((n) => nodeById.set(String(n.id), n));
      (view.edges || []).forEach((e) => edgeIds.add(String(e.id)));
      const elements = [...(view.nodes || []).map(elementForNode), ...(view.edges || []).map(elementForEdge)];
      cy = cytoscape({container: document.getElementById('cy'), elements, style: [{selector:'node',style:{'background-color':'data(color)','label':'data(label)','color':'#e7edf4','font-size':10,'text-wrap':'wrap','text-max-width':120,'text-valign':'bottom','text-margin-y':7,'width':18,'height':18,'border-width':2,'border-color':'data(border)'}},{selector:'edge',style:{'width':1.5,'line-color':'data(color)','target-arrow-color':'data(color)','target-arrow-shape':'triangle','curve-style':'bezier','label':'data(kind)','font-size':8,'color':'#9fb0c0','text-background-color':'#0c1117','text-background-opacity':.8}},{selector:'.filtered',style:{display:'none'}},{selector:':selected',style:{'overlay-color':'#fff','overlay-opacity':.12}}], layout:{name:'cose',animate:false,padding:40}, wheelSensitivity:.25});
      cy.on('tap', 'node,edge', (event) => event.target.group() === 'nodes' ? showNode(nodeById.get(event.target.id())) : showEdge(event.target.data()));
    }
    rebuildAccessibleNodes();
    updateSummary();
    document.getElementById('empty').hidden = nodeById.size !== 0;
  };
  const expandSelected = async () => {
    if (!liveToken || !selectedNode) return;
    expandButton.disabled = true;
    try {
      const params = new URLSearchParams({token: liveToken, root: String(selectedNode.id), depth: '1', direction: 'out'});
      const response = await fetch(`/api/graph?${params}`);
      if (!response.ok) throw new Error(`Graph expansion failed (${response.status})`);
      addView(await response.json(), true);
      showNode(nodeById.get(String(selectedNode.id)) || selectedNode);
    } catch (error) {
      details.innerHTML = `<p class="muted">${esc(error.message)}</p>`;
    } finally {
      expandButton.disabled = false;
    }
  };
  const loadView = async () => {
    if (!liveToken) return embedded;
    const response = await fetch(`/api/graph?token=${encodeURIComponent(liveToken)}`);
    if (!response.ok) throw new Error(`Live GraphView request failed (${response.status})`);
    return response.json();
  };
  const start = async () => {
    try {
      addView(await loadView(), false);
    } catch (error) {
      addView(embedded, false);
      details.innerHTML = `<p class="muted">${esc(error.message)}</p>`;
    }
    document.getElementById('fit').onclick = () => cy.fit(undefined, 40);
    document.getElementById('expand').onclick = expandSelected;
    document.getElementById('reset').onclick = () => { cy.elements().removeClass('filtered'); cy.layout({name:'cose',animate:false,padding:40}).run(); };
    const viewKey = `cidx-view:${view.schema || 'unknown'}:${view.request?.root || view.request?.query || 'empty'}`;
    document.getElementById('save').onclick = () => { localStorage.setItem(viewKey, JSON.stringify({positions: cy.nodes().reduce((p, n) => ({...p, [n.id()]: n.position()}), {}), zoom: cy.zoom(), pan: cy.pan()})); };
    document.getElementById('restore').onclick = () => { try { const saved = JSON.parse(localStorage.getItem(viewKey) || 'null'); if (!saved) return; cy.nodes().positions((n) => saved.positions?.[n.id()] || n.position()); if (saved.zoom) cy.zoom(saved.zoom); if (saved.pan) cy.pan(saved.pan); } catch (_) {} };
    document.getElementById('search').oninput = (event) => { const query = event.target.value.toLowerCase(); cy.nodes().forEach((n) => n.toggleClass('filtered', query && !String(n.data('label')).toLowerCase().includes(query))); };
  };
  start();
})();
}
