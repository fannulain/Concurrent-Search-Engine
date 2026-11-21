(() => {
  const els = {
    wsBadge: document.getElementById("wsBadge"),
    refreshBtn: document.getElementById("refreshBtn"),
    serverStatus: document.getElementById("serverStatus"),
    ollamaStatus: document.getElementById("ollamaStatus"),
    docsCount: document.getElementById("docsCount"),
    buildTime: document.getElementById("buildTime"),
    threadsUsed: document.getElementById("threadsUsed"),
    indexForm: document.getElementById("indexForm"),
    threadsInput: document.getElementById("threadsInput"),
    buildIndexBtn: document.getElementById("buildIndexBtn"),
    indexMessage: document.getElementById("indexMessage"),
    searchForm: document.getElementById("searchForm"),
    queryInput: document.getElementById("queryInput"),
    limitSelect: document.getElementById("limitSelect"),
    modeSelect: document.getElementById("modeSelect"),
    searchBtn: document.getElementById("searchBtn"),
    searchError: document.getElementById("searchError"),
    pipelineSummary: document.getElementById("pipelineSummary"),
    stageExpansion: document.getElementById("stageExpansion"),
    stageExpansionMeta: document.getElementById("stageExpansionMeta"),
    expandedTermsList: document.getElementById("expandedTermsList"),
    stageSearch: document.getElementById("stageSearch"),
    stageSearchMeta: document.getElementById("stageSearchMeta"),
    stageDone: document.getElementById("stageDone"),
    stageDoneMeta: document.getElementById("stageDoneMeta"),
    fallbackBanner: document.getElementById("fallbackBanner"),
    resultsMeta: document.getElementById("resultsMeta"),
    resultsList: document.getElementById("resultsList"),
  };

  let ws = null;
  let wsReconnectTimer = null;
  let activePipelineStart = 0;
  let activeHighlightTerms = [];
  let searchInFlight = false;

  function setStatusText(el, ok, text) {
    el.textContent = text;
    el.className = "stat-value " + (ok ? "ok" : "fail");
  }

  function showFeedback(el, text, variant) {
    el.textContent = text;
    el.className = "feedback" + (variant ? " feedback-" + variant : "");
  }

  function hideFeedback(el) {
    el.textContent = "";
    el.className = "feedback hidden";
  }

  function setStageState(cardEl, metaEl, state, stateLabel, metaText) {
    cardEl.className = "stage-card stage-" + state;
    const stateSpan = cardEl.querySelector(".stage-state");
    if (stateSpan) stateSpan.textContent = stateLabel;
    metaEl.textContent = metaText;
  }

  function clearChildren(el) {
    while (el.firstChild) {
      el.removeChild(el.firstChild);
    }
  }

  function renderExpandedTerms(terms) {
    clearChildren(els.expandedTermsList);
    for (const term of terms) {
      const chip = document.createElement("span");
      chip.className = "chip";
      chip.textContent = term;
      els.expandedTermsList.appendChild(chip);
    }
  }

  function resetPipeline(mode) {
    hideFeedback(els.fallbackBanner);
    hideFeedback(els.searchError);
    clearChildren(els.expandedTermsList);

    if (mode === "stream") {
      setStageState(els.stageExpansion, els.stageExpansionMeta, "running", "running", "Calling Ollama /api/generate...");
      setStageState(els.stageSearch, els.stageSearchMeta, "pending", "pending", "Waiting for expansion");
      setStageState(els.stageDone, els.stageDoneMeta, "pending", "pending", "In progress");
      els.pipelineSummary.textContent = "Streaming search pipeline...";
    } else {
      setStageState(els.stageExpansion, els.stageExpansionMeta, "pending", "skipped", "Bypassed in REST keyword mode");
      setStageState(els.stageSearch, els.stageSearchMeta, "running", "running", "Querying inverted index...");
      setStageState(els.stageDone, els.stageDoneMeta, "pending", "pending", "In progress");
      els.pipelineSummary.textContent = "Executing REST keyword search...";
    }
  }

  function escapeRegex(value) {
    return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  }

  function appendHighlightedSnippet(container, snippetText, terms) {
    if (!snippetText) {
      container.textContent = "(no snippet available)";
      return;
    }

    const validTerms = terms.filter((t) => t && t.length >= 2);
    if (validTerms.length === 0) {
      container.textContent = snippetText;
      return;
    }

    const pattern = new RegExp("\\b(" + validTerms.map(escapeRegex).join("|") + ")\\b", "gi");
    let lastIndex = 0;
    let match;

    while ((match = pattern.exec(snippetText)) !== null) {
      if (match.index > lastIndex) {
        container.appendChild(document.createTextNode(snippetText.slice(lastIndex, match.index)));
      }
      const mark = document.createElement("mark");
      mark.textContent = match[0];
      container.appendChild(mark);
      lastIndex = pattern.lastIndex;
    }

    if (lastIndex < snippetText.length) {
      container.appendChild(document.createTextNode(snippetText.slice(lastIndex)));
    }
  }

  function renderResults(results, timeMs) {
    clearChildren(els.resultsList);
    els.resultsMeta.textContent = results.length + " result(s) in " + timeMs + " ms";

    if (results.length === 0) {
      const empty = document.createElement("div");
      empty.className = "muted";
      empty.textContent = "No matching documents found. Make sure the index has been built.";
      els.resultsList.appendChild(empty);
      return;
    }

    results.forEach((item, index) => {
      const card = document.createElement("article");
      card.className = "result-item";

      const top = document.createElement("div");
      top.className = "result-top";

      const title = document.createElement("div");
      title.className = "result-title";

      const rank = document.createElement("span");
      rank.className = "result-rank";
      rank.textContent = "#" + (index + 1);

      const doc = document.createElement("span");
      doc.className = "result-doc";
      doc.textContent = item.doc_id;

      title.appendChild(rank);
      title.appendChild(doc);

      const scoreWrap = document.createElement("div");
      scoreWrap.className = "result-score-wrap";

      const barTrack = document.createElement("div");
      barTrack.className = "score-bar-track";

      const barFill = document.createElement("div");
      barFill.className = "score-bar-fill";
      const pct = Math.max(0, Math.min(100, Math.round((Number(item.score) || 0) * 100)));
      barFill.style.width = pct + "%";
      barTrack.appendChild(barFill);

      const scoreText = document.createElement("span");
      scoreText.className = "result-score";
      scoreText.textContent = Number(item.score || 0).toFixed(3);

      scoreWrap.appendChild(barTrack);
      scoreWrap.appendChild(scoreText);

      top.appendChild(title);
      top.appendChild(scoreWrap);

      const snippet = document.createElement("p");
      snippet.className = "result-snippet";
      appendHighlightedSnippet(snippet, item.snippet || "", activeHighlightTerms);

      card.appendChild(top);
      card.appendChild(snippet);
      els.resultsList.appendChild(card);
    });
  }

  async function refreshStatus() {
    try {
      const [healthRes, statsRes] = await Promise.all([fetch("/api/health"), fetch("/api/stats")]);
      const health = await healthRes.json();
      setStatusText(els.serverStatus, healthRes.ok && health.status === "ok", healthRes.ok ? "OK" : "HTTP " + healthRes.status);
      setStatusText(els.ollamaStatus, health.ollama === true, health.ollama ? "available" : "unavailable");

      if (statsRes.ok) {
        const stats = await statsRes.json();
        els.docsCount.textContent = Number(stats.documents || 0).toLocaleString();
        els.buildTime.textContent = (stats.index_build_time_ms || 0) + " ms";
        els.threadsUsed.textContent = String(stats.threads_used || 0);
      }
    } catch (err) {
      setStatusText(els.serverStatus, false, "unreachable");
      setStatusText(els.ollamaStatus, false, "unknown");
    }
  }

  function updateWsBadge(state, text) {
    els.wsBadge.className = "badge badge-" + state;
    els.wsBadge.textContent = text;
  }

  function connectWebSocket() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      return;
    }

    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const wsUrl = protocol + "//" + window.location.host + "/ws/search";
    updateWsBadge("pending", "WS: connecting...");

    ws = new WebSocket(wsUrl);

    ws.addEventListener("open", () => {
      updateWsBadge("ok", "WS: connected");
    });

    ws.addEventListener("close", () => {
      updateWsBadge("fail", "WS: disconnected");
      if (searchInFlight) {
        searchInFlight = false;
        els.searchBtn.disabled = false;
        showFeedback(els.searchError, "WebSocket connection closed during search. Please retry.", "error");
        if (els.stageExpansion.classList.contains("stage-running")) {
          setStageState(els.stageExpansion, els.stageExpansionMeta, "failed", "failed", "Connection lost");
        }
        if (els.stageSearch.classList.contains("stage-running") || els.stageSearch.classList.contains("stage-pending")) {
          setStageState(els.stageSearch, els.stageSearchMeta, "failed", "failed", "Aborted");
        }
        setStageState(els.stageDone, els.stageDoneMeta, "failed", "failed", "Disconnected");
        els.pipelineSummary.textContent = "Connection lost";
      }
      clearTimeout(wsReconnectTimer);
      wsReconnectTimer = setTimeout(connectWebSocket, 3000);
    });

    ws.addEventListener("error", () => {
      updateWsBadge("fail", "WS: error");
    });

    ws.addEventListener("message", (event) => {
      let msg;
      try {
        msg = JSON.parse(event.data);
      } catch (e) {
        return;
      }

      const elapsedMs = Math.round(performance.now() - activePipelineStart);

      if (msg.stage === "error") {
        const errMsg = (msg.data && msg.data.message) || "Invalid search request";
        showFeedback(els.searchError, errMsg, "error");
        setStageState(els.stageExpansion, els.stageExpansionMeta, "failed", "failed", errMsg);
        setStageState(els.stageSearch, els.stageSearchMeta, "failed", "failed", "Aborted");
        setStageState(els.stageDone, els.stageDoneMeta, "failed", "failed", "Error");
        els.pipelineSummary.textContent = "Pipeline failed";
        searchInFlight = false;
        els.searchBtn.disabled = false;
        return;
      }

      if (msg.stage === "query_expansion") {
        const data = msg.data || {};
        const expanded = Array.isArray(data.expanded_terms) ? data.expanded_terms : [];
        const baseTerms = (data.original || "").toLowerCase().match(/[a-z]{2,}/g) || [];
        activeHighlightTerms = Array.from(new Set(baseTerms.concat(expanded)));

        renderExpandedTerms(expanded);
        if (data.error) {
          setStageState(els.stageExpansion, els.stageExpansionMeta, "fallback", "fallback",
            "Fallback at " + elapsedMs + " ms (original terms only)");
          showFeedback(els.fallbackBanner, "LLM fallback: " + data.error, "warn");
        } else {
          setStageState(els.stageExpansion, els.stageExpansionMeta, "completed", "completed",
            expanded.length + " term(s) added in " + elapsedMs + " ms");
        }
        setStageState(els.stageSearch, els.stageSearchMeta, "running", "running", "Searching inverted index...");
        return;
      }

      if (msg.stage === "keyword_search") {
        const data = msg.data || {};
        const results = Array.isArray(data.results) ? data.results : [];
        const searchMs = data.time_ms || 0;
        setStageState(els.stageSearch, els.stageSearchMeta, "completed", "completed",
          results.length + " result(s) in " + searchMs + " ms");
        renderResults(results, searchMs);
        return;
      }

      if (msg.stage === "done") {
        setStageState(els.stageDone, els.stageDoneMeta, "completed", "completed",
          "Total elapsed: " + elapsedMs + " ms");
        els.pipelineSummary.textContent = "Completed in " + elapsedMs + " ms";
        searchInFlight = false;
        els.searchBtn.disabled = false;
      }
    });
  }

  async function runRestSearch(query, limit) {
    const start = performance.now();
    try {
      const res = await fetch("/api/search", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ query, mode: "keyword", limit }),
      });
      const body = await res.json();
      const totalMs = Math.round(performance.now() - start);

      if (!res.ok) {
        const errMsg = body.error || "HTTP " + res.status;
        showFeedback(els.searchError, errMsg, "error");
        setStageState(els.stageSearch, els.stageSearchMeta, "failed", "failed", errMsg);
        setStageState(els.stageDone, els.stageDoneMeta, "failed", "failed", "Error");
        els.pipelineSummary.textContent = "Search failed";
        return;
      }

      const results = Array.isArray(body.results) ? body.results : [];
      const searchMs = body.time_ms || 0;
      activeHighlightTerms = query.toLowerCase().match(/[a-z]{2,}/g) || [];
      setStageState(els.stageSearch, els.stageSearchMeta, "completed", "completed",
        results.length + " result(s) in " + searchMs + " ms");
      setStageState(els.stageDone, els.stageDoneMeta, "completed", "completed",
        "Total elapsed: " + totalMs + " ms");
      els.pipelineSummary.textContent = "Completed in " + totalMs + " ms";
      renderResults(results, searchMs);
    } catch (err) {
      showFeedback(els.searchError, "Failed to reach search server", "error");
      setStageState(els.stageSearch, els.stageSearchMeta, "failed", "failed", "Network error");
      setStageState(els.stageDone, els.stageDoneMeta, "failed", "failed", "Error");
      els.pipelineSummary.textContent = "Search failed";
    } finally {
      searchInFlight = false;
      els.searchBtn.disabled = false;
    }
  }

  els.indexForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    const threads = Number.parseInt(els.threadsInput.value, 10);
    if (!Number.isInteger(threads) || threads < 1 || threads > 64) {
      showFeedback(els.indexMessage, "Threads must be an integer between 1 and 64.", "error");
      return;
    }

    els.buildIndexBtn.disabled = true;
    showFeedback(els.indexMessage, "Building inverted index with " + threads + " thread(s)...", "");

    try {
      const res = await fetch("/api/index", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ threads }),
      });
      const body = await res.json();
      if (!res.ok) {
        showFeedback(els.indexMessage, body.error || "Index build failed (HTTP " + res.status + ")", "error");
      } else {
        showFeedback(
          els.indexMessage,
          "Indexed " + Number(body.documents_indexed || 0).toLocaleString() + " documents in " + body.time_ms + " ms.",
          "ok"
        );
        await refreshStatus();
      }
    } catch (err) {
      showFeedback(els.indexMessage, "Failed to contact server while indexing.", "error");
    } finally {
      els.buildIndexBtn.disabled = false;
    }
  });

  els.searchForm.addEventListener("submit", (e) => {
    e.preventDefault();
    const query = els.queryInput.value.trim();
    const limit = Number.parseInt(els.limitSelect.value, 10) || 10;
    const mode = els.modeSelect.value;

    if (!query) {
      showFeedback(els.searchError, "Please enter a non-empty query.", "error");
      return;
    }

    searchInFlight = true;
    els.searchBtn.disabled = true;
    activePipelineStart = performance.now();
    resetPipeline(mode);

    if (mode === "keyword") {
      runRestSearch(query, limit);
      return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
      showFeedback(els.searchError, "WebSocket is not connected yet. Try again or switch to REST Keyword mode.", "error");
      setStageState(els.stageExpansion, els.stageExpansionMeta, "failed", "failed", "WebSocket disconnected");
      setStageState(els.stageDone, els.stageDoneMeta, "failed", "failed", "Error");
      els.pipelineSummary.textContent = "WebSocket unavailable";
      searchInFlight = false;
      els.searchBtn.disabled = false;
      connectWebSocket();
      return;
    }

    ws.send(JSON.stringify({ query, limit }));
  });

  els.refreshBtn.addEventListener("click", refreshStatus);

  refreshStatus();
  connectWebSocket();
})();
