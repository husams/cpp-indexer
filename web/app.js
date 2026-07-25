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
    // The live explorer keeps growing one canvas across many merged slices
    // (HSE-92 progressive patches); a session must never present a merged
    // view as more current than its weakest contributing slice, so freshness
    // is tracked with the same weakest-wins rule as per-element status.
    if (left.freshness || right.freshness) merged.freshness = weakest(left.freshness, right.freshness, freshnessRank);
    if (left.identity || right.identity) {
      merged.identity = {...left.identity, ...right.identity};
      if (left.identity?.freshness || right.identity?.freshness) merged.identity.freshness = weakest(left.identity?.freshness, right.identity?.freshness, freshnessRank);
    }
    if (sitesUsed !== undefined) merged.sites_used = sitesUsed;
    return merged;
  };
  const budgetValue = (metadata, request, name) => {
    const value = metadata?.[name] ?? request?.[name];
    return Number.isFinite(Number(value)) && Number(value) >= 0 ? Number(value) : Number.POSITIVE_INFINITY;
  };
  const markTruncated = (element, evidence = false) => {
    const status = mergeStatus(element.status, {truncated: true, ...(evidence ? {evidence_truncated: true} : {})});
    const marked = {...element, status};
    if (evidence) marked.evidence = mergeFlags(element.evidence, {truncated: true, sites_truncated: true});
    return marked;
  };
  // `options.cumulative` distinguishes two merge scenarios that need
  // different budget semantics (HSE-92 review):
  //  - default (false): merging one "expand" result into the canvas. Each
  //    side is independently bounded, and the merge stays bounded by the
  //    SMALLER of the two -- unchanged, pre-existing behavior.
  //  - true: merging a continuation "Load more" page, whose whole purpose
  //    is to grow the canvas beyond any single page's own budget. Budgets
  //    are SUMMED (not minimized) and the sum is persisted back into the
  //    merged metadata, so a third, fourth, ... page keeps growing the
  //    allowance instead of being re-capped to one page's size.
  const mergeSlices = (left, right, options = {}) => {
    const cumulative = options.cumulative === true;
    const combineBudgets = cumulative ? (a, b) => a + b : Math.min;
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
    const allNodes = [...nodes.values()];
    const allEdges = [...edges.values()];
    const nodeBudget = combineBudgets(
      budgetValue(left.metadata, left.request, 'node_budget'),
      budgetValue(right.metadata, right.request, 'node_budget'));
    const edgeBudget = combineBudgets(
      budgetValue(left.metadata, left.request, 'edge_budget'),
      budgetValue(right.metadata, right.request, 'edge_budget'));
    const siteBudget = combineBudgets(
      budgetValue(left.metadata, left.request, 'site_budget'),
      budgetValue(right.metadata, right.request, 'site_budget'));
    const nodeOverflow = allNodes.length > nodeBudget;
    const mergedNodes = allNodes.slice(0, nodeBudget);
    const retainedNodeIds = new Set(mergedNodes.map((node) => String(node.id)));
    const eligibleEdges = allEdges.filter((edge) => retainedNodeIds.has(String(edge.source)) && retainedNodeIds.has(String(edge.target)));
    const edgeOverflow = allEdges.length > edgeBudget || eligibleEdges.length < allEdges.length;
    const mergedEdges = eligibleEdges.slice(0, edgeBudget);
    let sitesUsed = 0;
    let siteOverflow = false;
    const limitedEdges = mergedEdges.map((edge) => {
      const sites = edge.sites || [];
      const remaining = Math.max(0, siteBudget - sitesUsed);
      const retainedSites = sites.slice(0, remaining);
      sitesUsed += retainedSites.length;
      if (retainedSites.length < sites.length) siteOverflow = true;
      if (retainedSites.length === sites.length) return edge;
      return markTruncated({...edge, sites: retainedSites}, true);
    });
    const budgetTruncated = nodeOverflow || edgeOverflow || siteOverflow;
    const retainedNodes = nodeOverflow ? mergedNodes.map((node) => markTruncated(node)) : mergedNodes;
    const retainedEdges = edgeOverflow ? limitedEdges.map((edge) => markTruncated(edge, edge.status?.evidence_truncated === true)) : limitedEdges;
    const elementTruncated = [...retainedNodes, ...retainedEdges].some((element) => element.status?.truncated === true);
    const evidenceTruncated = retainedEdges.some((edge) => edge.status?.evidence_truncated === true || edge.evidence?.sites_truncated === true);
    const metadata = mergeMetadata(left.metadata, right.metadata, sitesUsed);
    if (cumulative) {
      // Carry the grown budget forward so the NEXT continuation merge sums
      // against it, not against a single page's original small budget.
      if (Number.isFinite(nodeBudget)) metadata.node_budget = nodeBudget;
      if (Number.isFinite(edgeBudget)) metadata.edge_budget = edgeBudget;
      if (Number.isFinite(siteBudget)) metadata.site_budget = siteBudget;
    }
    metadata.truncated = Boolean(metadata.truncated || elementTruncated || budgetTruncated);
    metadata.evidence_truncated = Boolean(metadata.evidence_truncated || evidenceTruncated);
    metadata.continuation.available = Boolean(metadata.continuation.available || metadata.truncated);
    if (metadata.truncated && metadata.continuation.reason !== 'byte_budget') metadata.continuation.reason = 'budget';
    return {...left, metadata, nodes: retainedNodes, edges: retainedEdges};
  };
  // Session-wide freshness tracker (HSE-92): a workspace becoming stale must
  // be surfaced immediately and never silently upgraded back to "current" by
  // a later merge; only an explicit full requery (not a merge) may clear it.
  const trackFreshness = (previous, next) => weakest(previous, next, freshnessRank);
  // Pure grouping decision (HSE-92 review: the grouping ALGORITHM, not just
  // the node-filter it was previously conflated with, needs its own
  // coverage). Takes plain node data objects (as returned by a Cytoscape
  // element's .data()) and a group-by key ('kind'|'file'|'component'), and
  // returns which nodes should be placed under which synthetic compound
  // parent -- singleton groups are left ungrouped, matching applyGrouping()
  // in the browser IIFE below, which is the only caller that actually
  // drives Cytoscape with this plan.
  const computeGroups = (nodesData, key) => {
    if (!key) return [];
    const groups = new Map();
    (nodesData || []).forEach((data) => {
      if (!data || data.cidxGroup) return;
      const raw = key === 'kind' ? data.kind : key === 'file' ? data.file : data.component;
      const groupKey = raw || `(no ${key})`;
      if (!groups.has(groupKey)) groups.set(groupKey, []);
      groups.get(groupKey).push(data.id);
    });
    const plan = [];
    groups.forEach((memberIds, groupKey) => {
      if (memberIds.length < 2) return;
      plan.push({groupId: `cidx-group:${key}:${groupKey}`, groupKey, memberIds});
    });
    return plan;
  };
  return {mergeSlices, trackFreshness, computeGroups};
})();

if (typeof module !== 'undefined') module.exports = graphState;
if (typeof document !== 'undefined') {
(() => {
  const embedded = window.CIDX_GRAPH_VIEW || {nodes: [], edges: [], metadata: {}};
  const offline = window.CIDX_OFFLINE === true;
  const liveToken = offline ? null : new URLSearchParams(window.location.search).get('token');
  let view = embedded;
  let cy;
  let selectedNode;
  let sessionFreshness = 'current';
  let currentParams = {};
  let history = [];
  let historyIndex = -1;
  let lastContinuationToken = null;
  const nodeById = new Map();
  const edgeIds = new Set();
  const details = document.getElementById('details');
  const title = document.getElementById('selection-title');
  const expandButton = document.getElementById('expand');
  const expandOutButton = document.getElementById('expand-out');
  const expandInButton = document.getElementById('expand-in');
  const accessible = document.getElementById('accessible-nodes');
  const liveControls = document.getElementById('live-controls');
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const badge = (name, cls) => `<span class="badge ${cls || ''}">${esc(name)}</span>`;
  const statusBadges = (s = {}) => Object.entries(s)
    .filter(([k, v]) => v === true || v === 'partial' || v === 'unknown' || v === 'stale' || v === 'truncated' || v === 'external')
    .map(([k, v]) => badge(`${k}${v === true ? '' : `: ${v}`}`, k === 'external' ? 'external' : k))
    .join('');
  const sourceLocation = (item) => item?.location || (item?.file ? `${item.file}:${item.line ?? ''}:${item.col ?? ''}` : 'No location');
  const copyButton = (text) => liveToken && text
    ? `<button type="button" class="copy-location" data-copy="${esc(text)}">Copy location</button>`
    : '';
  const wireCopyButtons = () => {
    details.querySelectorAll('.copy-location').forEach((button) => {
      button.onclick = () => { navigator.clipboard?.writeText(button.dataset.copy || '').catch(() => {}); };
    });
  };
  const updateStaleBanner = () => {
    const banner = document.getElementById('stale-banner');
    if (banner) banner.hidden = sessionFreshness === 'current';
  };
  const trackViewFreshness = (nextView) => {
    const freshness = nextView.metadata?.identity?.freshness || nextView.identity?.freshness || nextView.metadata?.index?.freshness;
    if (freshness) sessionFreshness = graphState.trackFreshness(sessionFreshness, freshness);
    updateStaleBanner();
  };
  const updateSummary = () => {
    const status = view.status || view.metadata?.status || 'unknown';
    const markers = view.markers || view.metadata?.markers || [];
    const request = view.request || {};
    const budgets = [request.node_budget, request.edge_budget, request.site_budget, request.byte_budget]
      .map((value) => Number.isFinite(Number(value)) ? Number(value) : null);
    document.getElementById('summary').textContent = `${view.nodes?.length || 0} nodes · ${view.edges?.length || 0} edges · ${status}${markers.length ? ` · ${markers.join(', ')}` : ''}${view.metadata?.truncated ? ' · bounded/truncated' : ''}`;
    const identity = view.metadata?.identity || {};
    const freshness = view.metadata?.index?.freshness || identity.freshness || 'unverifiable';
    document.getElementById('identity').textContent = `${identity.workspace || 'workspace unknown'} · ${identity.index || 'index unknown'} · ${freshness} · query ${view.query_identity || 'unknown'} · result ${view.result_id || 'unknown'}`;
    const canvasStatus = document.getElementById('canvas-status');
    canvasStatus.innerHTML = `${badge(status, status)} ${markers.map((marker) => badge(marker, marker)).join('')} ${budgets.some((value) => value !== null) ? `<span class="budget">budgets ${budgets.filter((value) => value !== null).join(' / ')}</span>` : ''}`;
    const loadMoreButton = document.getElementById('load-more');
    if (loadMoreButton) {
      const continuation = view.metadata?.continuation;
      lastContinuationToken = continuation?.available && continuation?.token ? continuation.token : null;
      loadMoreButton.hidden = !lastContinuationToken;
    }
  };
  const showSnapshot = () => {
    selectedNode = undefined;
    expandButton.disabled = true;
    if (expandOutButton) expandOutButton.disabled = true;
    if (expandInButton) expandInButton.disabled = true;
    title.textContent = 'Snapshot status';
    const identity = view.identity || view.metadata?.identity || {};
    const request = view.request || {};
    details.innerHTML = `<dl><dt>Status</dt><dd>${statusBadges({status: view.status, ...(Object.fromEntries((view.markers || []).map((marker) => [marker, true])))}) || 'unknown'}</dd><dt>Query</dt><dd><code>${esc(view.query_identity || 'unknown')}</code></dd><dt>Result</dt><dd><code>${esc(view.result_id || 'unknown')}</code></dd><dt>Input</dt><dd>${esc(request.input_kind || 'unknown')} · <code>${esc(request.input || '')}</code></dd><dt>Fact sets</dt><dd>${esc((identity.fact_sets || []).join(', ') || 'unknown')}</dd><dt>Budgets</dt><dd>${esc(JSON.stringify({nodes: request.node_budget, edges: request.edge_budget, sites: request.site_budget, bytes: request.byte_budget}))}</dd></dl>`;
  };
  const showNode = (n) => {
    selectedNode = n;
    expandButton.disabled = !liveToken;
    if (expandOutButton) expandOutButton.disabled = !liveToken;
    if (expandInButton) expandInButton.disabled = !liveToken;
    title.textContent = n.name || n.usr || 'Node';
    const evidence = n.evidence?.location || 'No bounded evidence attached';
    details.innerHTML = `<dl><dt>Canonical id</dt><dd><code>${esc(n.id)}</code></dd><dt>USR</dt><dd><code>${esc(n.usr)}</code></dd><dt>Universe/key</dt><dd><code>${esc(`${n.semantic_universe || ''}/${n.identity_key || ''}`)}</code></dd><dt>Kind</dt><dd>${esc(n.kind)}</dd><dt>Component</dt><dd>${esc(n.component || 'unknown')}</dd><dt>Location</dt><dd>${esc(sourceLocation(n))} ${copyButton(sourceLocation(n))}</dd><dt>Status</dt><dd>${statusBadges(n.status) || 'No status flags'}</dd><dt>Evidence</dt><dd>${esc(evidence)}</dd></dl>`;
    wireCopyButtons();
  };
  const showEdge = (e) => {
    selectedNode = undefined;
    expandButton.disabled = true;
    if (expandOutButton) expandOutButton.disabled = true;
    if (expandInButton) expandInButton.disabled = true;
    title.textContent = `${e.kind} relation`;
    const evidenceTruncated = Boolean(e.status?.evidence_truncated || e.evidence?.sites_truncated);
    const loadEvidenceButton = evidenceTruncated && liveToken
      ? '<button type="button" id="load-evidence">Load all evidence</button>' : '';
    details.innerHTML = `<dl><dt>Canonical id</dt><dd><code>${esc(e.id)}</code></dd><dt>From</dt><dd><code>${esc(e.source)}</code></dd><dt>To</dt><dd><code>${esc(e.target)}</code></dd><dt>Kind/count</dt><dd>${esc(e.kind)} · ${esc(e.count)}</dd><dt>Status</dt><dd>${statusBadges(e.status) || 'No status flags'}</dd><dt>Evidence</dt><dd>${(e.sites || []).map((s) => esc(sourceLocation(s))).join('<br>') || 'No site evidence'} ${loadEvidenceButton}</dd></dl>`;
    if (evidenceTruncated && liveToken) {
      document.getElementById('load-evidence').onclick = async () => {
        try {
          const query = new URLSearchParams({token: liveToken, edge: String(e.id), site_limit: '5000'});
          const response = await fetch(`/api/evidence?${query}`);
          if (!response.ok) throw new Error(`Evidence request failed (${response.status})`);
          const payload = await response.json();
          const list = (payload.sites || []).map((s) => esc(sourceLocation(s))).join('<br>');
          document.getElementById('load-evidence').outerHTML = `<div>${list || 'No additional sites'}${payload.truncated ? ' <span class="badge truncated">still truncated</span>' : ''}</div>`;
        } catch (error) {
          details.insertAdjacentHTML('beforeend', `<p class="muted">${esc(error.message)}</p>`);
        }
      };
    }
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
  // HSE-92 grouping: purely a presentation facet over already-delivered,
  // already-authoritative node fields (component/file/kind) -- it never
  // asks the server a new question or changes the semantic result.
  const applyGrouping = () => {
    const key = document.getElementById('group-by')?.value;
    if (!cy) return;
    cy.nodes('[^cidxGroup]').forEach((element) => element.move({parent: null}));
    cy.$('.cidx-group').remove();
    if (!key) return;
    const primaryNodes = cy.nodes('[^cidxGroup]');
    const nodesData = primaryNodes.map((element) => element.data());
    const byId = new Map(primaryNodes.map((element) => [element.id(), element]));
    graphState.computeGroups(nodesData, key).forEach(({groupId, groupKey, memberIds}) => {
      cy.add({group: 'nodes', data: {id: groupId, label: groupKey, cidxGroup: true}, classes: 'cidx-group'});
      memberIds.forEach((id) => byId.get(id)?.move({parent: groupId}));
    });
  };
  const addView = (next, append, mergeOptions = {}) => {
    if (append) {
      const merged = graphState.mergeSlices(view, next, mergeOptions);
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
      cy = cytoscape({container: document.getElementById('cy'), elements, style: [{selector:'node',style:{'background-color':'data(color)','label':'data(label)','color':'#e7edf4','font-size':10,'text-wrap':'wrap','text-max-width':120,'text-valign':'bottom','text-margin-y':7,'width':18,'height':18,'border-width':2,'border-color':'data(border)'}},{selector:'.cidx-group',style:{'background-opacity':.08,'border-width':1,'border-color':'#3a4a5c','label':'data(label)','text-valign':'top','font-size':10,'color':'#8fa0b3'}},{selector:'edge',style:{'width':1.5,'line-color':'data(color)','target-arrow-color':'data(color)','target-arrow-shape':'triangle','curve-style':'bezier','label':'data(kind)','font-size':8,'color':'#9fb0c0','text-background-color':'#0c1117','text-background-opacity':.8}},{selector:'.filtered',style:{display:'none'}},{selector:':selected',style:{'overlay-color':'#fff','overlay-opacity':.12}}], layout:{name:'cose',animate:false,padding:40}, wheelSensitivity:.25});
      cy.on('tap', 'node,edge', (event) => event.target.group() === 'nodes' ? (event.target.data().cidxGroup ? undefined : showNode(nodeById.get(event.target.id()))) : showEdge(event.target.data()));
      applyGrouping();
    }
    trackViewFreshness(view);
    rebuildAccessibleNodes();
    updateSummary();
    if (!selectedNode) showSnapshot();
    document.getElementById('empty').hidden = nodeById.size !== 0;
  };
  // ---- HSE-92: typed live operations -------------------------------------
  const fetchGraph = async (params) => {
    const query = new URLSearchParams({token: liveToken, ...params});
    const response = await fetch(`/api/graph?${query}`);
    if (!response.ok) throw new Error(`Graph request failed (${response.status})`);
    return response.json();
  };
  const reportError = (error) => { details.innerHTML = `<p class="muted">${esc(error.message)}</p>`; };
  // Presentation (camera/layout) state is captured/restored separately from
  // the semantic request each history entry carries (HSE-92 review: history
  // must restore both, independently).
  const capturePresentation = () => cy ? {
    positions: cy.nodes().reduce((acc, n) => ({...acc, [n.id()]: n.position()}), {}),
    zoom: cy.zoom(), pan: cy.pan(),
  } : null;
  const restorePresentation = (presentation) => {
    if (!presentation || !cy) return;
    cy.nodes().positions((n) => presentation.positions?.[n.id()] || n.position());
    if (presentation.zoom) cy.zoom(presentation.zoom);
    if (presentation.pan) cy.pan(presentation.pan);
  };
  const renderBreadcrumbs = () => {
    const nav = document.getElementById('breadcrumbs');
    if (!nav) return;
    nav.replaceChildren();
    history.forEach((entry, index) => {
      if (index > 0) {
        const sep = document.createElement('span');
        sep.className = 'sep';
        sep.textContent = '›';
        nav.appendChild(sep);
      }
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = entry.label;
      if (index === historyIndex) button.setAttribute('aria-current', 'true');
      button.onclick = () => jumpTo(index);
      nav.appendChild(button);
    });
  };
  const updateHistoryButtons = () => {
    const back = document.getElementById('history-back');
    const forward = document.getElementById('history-forward');
    if (back) back.disabled = historyIndex <= 0;
    if (forward) forward.disabled = historyIndex >= history.length - 1;
  };
  // Fetch+render only -- used both for fresh navigation and for replaying a
  // history/breadcrumb entry (which manages historyIndex itself).
  const go = async (params, label, presentation, options = {}) => {
    if (!liveToken) return;
    try {
      const nextView = await fetchGraph(params);
      // Only reset the session-wide stale flag once a request has actually
      // SUCCEEDED, and reset it to 'current' immediately before folding in
      // the new response's own freshness (via addView -> trackViewFreshness)
      // -- an explicit refresh that itself comes back stale/unverifiable
      // must still show the banner, and a refresh that fails outright must
      // leave the stale state exactly as it was (HSE-92 review: do not
      // clear staleness before the refresh is confirmed successful).
      if (options.resetFreshness) sessionFreshness = 'current';
      addView(nextView, false);
      currentParams = params;
      restorePresentation(presentation);
      renderBreadcrumbs();
      updateHistoryButtons();
    } catch (error) {
      reportError(error);
    }
  };
  // Records the presentation snapshot of whatever is CURRENTLY shown onto
  // the history entry we are about to navigate away from, so it can be
  // restored later without disturbing the entry we are moving to.
  const captureCurrentPresentation = () => {
    if (history[historyIndex]) history[historyIndex].presentation = capturePresentation();
  };
  // Pushes a NEW history entry (truncating any forward history), matching
  // ordinary browser back/forward semantics for normalized queries.
  const navigate = (params, label) => {
    captureCurrentPresentation();
    history = history.slice(0, historyIndex + 1);
    history.push({label, params, presentation: null, append: false});
    historyIndex = history.length - 1;
    return go(params, label);
  };
  // Expansion is a MERGE onto the current canvas, not a replacement, but it
  // is still a normalized semantic action and must enter breadcrumbs/back-
  // forward like any other navigation (HSE-92 review). Pushes an `append`
  // history entry; see jumpTo() for how such entries are replayed.
  const pushExpand = async (params, label) => {
    if (!liveToken) return;
    try {
      const nextView = await fetchGraph(params);
      addView(nextView, true);
      captureCurrentPresentation();
      history = history.slice(0, historyIndex + 1);
      history.push({label, params, presentation: null, append: true});
      historyIndex = history.length - 1;
      renderBreadcrumbs();
      updateHistoryButtons();
    } catch (error) {
      reportError(error);
    }
  };
  // Jumping to an arbitrary history entry (back/forward/breadcrumb click)
  // replays every entry from the start up to and including `index`: each
  // non-append entry replaces the canvas, each append (expand) entry merges
  // onto whatever the replay has built so far. This reconstructs the exact
  // canvas at that point in history regardless of how many expand actions
  // came before it, then restores that entry's own presentation snapshot.
  const jumpTo = async (index) => {
    if (!liveToken || index < 0 || index >= history.length) return;
    captureCurrentPresentation();
    historyIndex = index;
    try {
      for (let step = 0; step <= index; step += 1) {
        const entry = history[step];
        const stepView = await fetchGraph(entry.params);
        addView(stepView, Boolean(entry.append));
      }
      currentParams = history[index].params;
      restorePresentation(history[index].presentation);
      renderBreadcrumbs();
      updateHistoryButtons();
    } catch (error) {
      reportError(error);
    }
  };
  const expandDirection = async (direction) => {
    if (!liveToken || !selectedNode) return;
    const id = String(selectedNode.id);
    const params = {root: id, depth: '1', direction};
    await pushExpand(params, `${direction} ${nodeById.get(id)?.name || id}`);
    showNode(nodeById.get(id) || selectedNode);
  };
  const runSearch = async () => {
    if (!liveToken) return;
    const input = document.getElementById('index-search');
    const results = document.getElementById('search-results');
    const text = input?.value.trim();
    if (!text || !results) return;
    try {
      const query = new URLSearchParams({token: liveToken, q: text, limit: '25'});
      const response = await fetch(`/api/search?${query}`);
      if (!response.ok) throw new Error(`Search failed (${response.status})`);
      const payload = await response.json();
      results.replaceChildren();
      (payload.matches || []).forEach((match) => {
        const button = document.createElement('button');
        button.type = 'button';
        button.textContent = `${match.name} (${match.kind}) — ${match.location}`;
        button.onclick = () => { results.hidden = true; navigate({root: match.id}, match.name); };
        results.appendChild(button);
      });
      if (!payload.matches?.length) {
        const empty = document.createElement('p');
        empty.className = 'muted';
        empty.textContent = 'No matches.';
        results.appendChild(empty);
      }
      results.hidden = false;
    } catch (error) {
      results.replaceChildren();
      const message = document.createElement('p');
      message.className = 'muted';
      message.textContent = error.message;
      results.appendChild(message);
      results.hidden = false;
    }
  };
  const SAVED_VIEWS_KEY = 'cidx-saved-views';
  const loadSavedViews = () => { try { return JSON.parse(localStorage.getItem(SAVED_VIEWS_KEY) || '[]'); } catch (_error) { return []; } };
  const persistSavedViews = (views) => { try { localStorage.setItem(SAVED_VIEWS_KEY, JSON.stringify(views)); } catch (_error) {} };
  const renderSavedViews = () => {
    const select = document.getElementById('saved-views');
    if (!select) return;
    select.replaceChildren();
    loadSavedViews().forEach((entry) => {
      const option = document.createElement('option');
      option.value = entry.name;
      option.textContent = entry.name;
      select.appendChild(option);
    });
  };
  const start = async () => {
    try {
      if (offline || !liveToken) {
        addView(embedded, false);
      } else {
        addView(await fetchGraph({}), false);
      }
    } catch (error) {
      addView(embedded, false);
      reportError(error);
    }
    if (liveToken) {
      history = [{label: 'initial', params: {}, presentation: null, append: false}];
      historyIndex = 0;
      renderBreadcrumbs();
      updateHistoryButtons();
      renderSavedViews();
      if (liveControls) liveControls.hidden = false;
    }
    document.getElementById('fit').onclick = () => cy.fit(undefined, 40);
    document.getElementById('expand').onclick = () => expandDirection('out');
    document.getElementById('reset').onclick = () => { cy.elements().removeClass('filtered'); cy.layout({name:'cose',animate:false,padding:40}).run(); };
    const viewKey = `cidx-view:${view.schema || 'unknown'}:${view.request?.root || view.request?.query || 'empty'}`;
    document.getElementById('save').onclick = () => { localStorage.setItem(viewKey, JSON.stringify({positions: cy.nodes().reduce((p, n) => ({...p, [n.id()]: n.position()}), {}), zoom: cy.zoom(), pan: cy.pan()})); };
    document.getElementById('restore').onclick = () => { try { const saved = JSON.parse(localStorage.getItem(viewKey) || 'null'); if (!saved) return; cy.nodes().positions((n) => saved.positions?.[n.id()] || n.position()); if (saved.zoom) cy.zoom(saved.zoom); if (saved.pan) cy.pan(saved.pan); } catch (_error) {} };
    document.getElementById('search').oninput = (event) => { const query = event.target.value.toLowerCase(); cy.nodes().forEach((n) => n.toggleClass('filtered', query && !String(n.data('label')).toLowerCase().includes(query))); };
    if (!liveToken) return;
    if (expandOutButton) expandOutButton.onclick = () => expandDirection('out');
    if (expandInButton) expandInButton.onclick = () => expandDirection('in');
    document.getElementById('history-back').onclick = () => {
      if (historyIndex <= 0) return;
      jumpTo(historyIndex - 1);
    };
    document.getElementById('history-forward').onclick = () => {
      if (historyIndex >= history.length - 1) return;
      jumpTo(historyIndex + 1);
    };
    document.getElementById('index-search-run').onclick = runSearch;
    document.getElementById('index-search').addEventListener('keydown', (event) => { if (event.key === 'Enter') runSearch(); });
    document.getElementById('load-more').onclick = async () => {
      if (!lastContinuationToken) return;
      try { addView(await fetchGraph({...currentParams, continuation: lastContinuationToken}), true, {cumulative: true}); }
      catch (error) { reportError(error); }
    };
    document.getElementById('group-by').onchange = applyGrouping;
    document.getElementById('apply-filters').onclick = () => {
      const params = {...currentParams};
      const fields = [
        ['filter-node-kind', 'node_kind'], ['filter-file', 'file'],
        ['filter-component', 'component'], ['filter-repository', 'repository'],
      ];
      fields.forEach(([elementId, param]) => {
        const value = document.getElementById(elementId)?.value.trim();
        if (value) params[param] = value; else delete params[param];
      });
      const status = document.getElementById('filter-status')?.value;
      if (status) params.status = status; else delete params.status;
      const applicability = document.getElementById('filter-applicability')?.value;
      if (applicability) params.applicability = applicability; else delete params.applicability;
      navigate(params, 'filtered');
    };
    document.getElementById('save-view').onclick = () => {
      const name = window.prompt('Name this saved view');
      if (!name) return;
      const views = loadSavedViews().filter((entry) => entry.name !== name);
      views.push({
        name, params: currentParams,
        presentation: cy ? {positions: cy.nodes().reduce((acc, n) => ({...acc, [n.id()]: n.position()}), {}), zoom: cy.zoom(), pan: cy.pan()} : null,
      });
      persistSavedViews(views);
      renderSavedViews();
    };
    document.getElementById('load-view').onclick = async () => {
      const select = document.getElementById('saved-views');
      const entry = loadSavedViews().find((item) => item.name === select?.value);
      if (!entry) return;
      await navigate(entry.params, entry.name);
      if (entry.presentation && cy) {
        cy.nodes().positions((n) => entry.presentation.positions?.[n.id()] || n.position());
        if (entry.presentation.zoom) cy.zoom(entry.presentation.zoom);
        if (entry.presentation.pan) cy.pan(entry.presentation.pan);
      }
    };
    document.getElementById('stale-refresh').onclick = () => {
      go(currentParams, history[historyIndex]?.label || 'refresh',
        history[historyIndex]?.presentation, {resetFreshness: true});
    };
    document.getElementById('shutdown').onclick = async () => {
      if (!window.confirm('Stop the local explorer server? This ends the session.')) return;
      try {
        await fetch(`/api/shutdown?token=${encodeURIComponent(liveToken)}`);
      } catch (_error) {
        // The connection closing as the server exits is the expected path.
      }
      details.innerHTML = '<p class="muted">Server stopped. You can close this tab.</p>';
    };
  };
  start();
})();
}
