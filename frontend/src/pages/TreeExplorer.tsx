import { useMemo, useState } from "react";
import { api, ApiError } from "../services/api";
import { useAsync } from "../hooks/useAsync";
import type { SearchPath, TreeNode } from "../types/api";
import BTreeView from "../components/BTreeView";
import { Badge, Button, Card, DataTable, EmptyState, ErrorBanner, Field, LoadingBlock, PageHeader, StatCard, inputClass } from "../components/ui";
import { formatInt } from "../utils/format";

/** Child slot chosen at an internal node for `key`: first index whose separator is greater than key. */
function chosenChild(n: TreeNode, key: number): number {
  let i = 0;
  while (i < n.keys.length && key >= n.keys[i]) i++;
  return i;
}

export default function TreeExplorer() {
  const [maxNodes, setMaxNodes] = useState(300);
  const tree = useAsync(() => api.tree(maxNodes), [maxNodes]);
  const validation = useAsync(() => api.validateTree(), []);
  const [keyText, setKeyText] = useState("");
  const [path, setPath] = useState<SearchPath | null>(null);
  const [pathError, setPathError] = useState<string | null>(null);
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [busy, setBusy] = useState(false);

  const t = tree.data;
  const byId = useMemo(() => new Map((t?.nodes ?? []).map((n) => [n.id, n])), [t]);
  const selected = selectedId !== null ? byId.get(selectedId) ?? null : null;
  const avgOcc = t && t.nodes.length ? t.nodes.reduce((s, n) => s + n.occupancy, 0) / t.nodes.length : 0;

  async function trace() {
    setPathError(null);
    if (!/^-?\d+$/.test(keyText.trim())) {
      setPathError("Enter an integer key.");
      return;
    }
    setBusy(true);
    try {
      const p = await api.searchPath(Number(keyText));
      setPath(p);
      setSelectedId(p.node_ids[p.node_ids.length - 1] ?? null);
    } catch (e) {
      setPath(null);
      setPathError(e instanceof ApiError ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }

  function refresh() {
    tree.reload();
    validation.reload();
    setPath(null);
  }

  return (
    <>
      <PageHeader
        title="B+ Tree Explorer"
        subtitle="The real index structure, exported by the C++ engine. Blue links trace the path a search takes; dashed arrows are the leaf sibling links used by range scans."
        actions={<Button onClick={refresh} busy={tree.loading}>Refresh</Button>}
      />
      {tree.error && !t && <ErrorBanner error={tree.error} onRetry={tree.reload} />}
      {!t && !tree.error && <LoadingBlock label="Loading the tree…" />}
      {t && (
        <div className="space-y-6">
          <div className="grid grid-cols-2 gap-4 lg:grid-cols-5">
            <StatCard label="Height" value={t.height} hint="levels, root to leaf" />
            <StatCard label="Keys" value={formatInt(t.key_count)} />
            <StatCard label="Nodes" value={formatInt(t.node_count)} hint={`${formatInt(t.leaf_count)} leaf · ${formatInt(t.internal_count)} internal`} />
            <StatCard label="Node capacity" value={t.max_keys} hint="max keys per node" />
            <StatCard
              label="Validation"
              value={validation.data ? (validation.data.valid ? "Valid" : "Invalid") : "…"}
              tone={validation.data ? (validation.data.valid ? "good" : "bad") : "default"}
              hint={validation.data ? (validation.data.valid ? "all invariants hold" : `${validation.data.violations.length} violation(s)`) : "checking"}
            />
          </div>

          {validation.data && !validation.data.valid && (
            <ErrorBanner error={`B+ Tree invariant violations: ${validation.data.violations.slice(0, 3).join(" | ")}`} />
          )}

          {t.key_count === 0 ? (
            <EmptyState title="The index is empty">Insert records in the Storage Explorer or generate sample data from the Overview.</EmptyState>
          ) : (
            <>
              <Card
                title="Search path"
                subtitle="Asks the engine which nodes a search for this key visits"
                actions={<span className="text-xs text-slate-500">Average node occupancy {(avgOcc * 100).toFixed(0)}%</span>}
              >
                <div className="flex flex-wrap items-end gap-3">
                  <Field label="Key">
                    <input
                      inputMode="numeric"
                      value={keyText}
                      onChange={(e) => setKeyText(e.target.value)}
                      onKeyDown={(e) => e.key === "Enter" && void trace()}
                      placeholder="e.g. 137"
                      className={`${inputClass} w-40 font-mono`}
                    />
                  </Field>
                  <Button variant="primary" onClick={() => void trace()} busy={busy}>Trace search path</Button>
                  {path && <Button variant="ghost" onClick={() => setPath(null)}>Clear</Button>}
                  <div className="ml-auto flex items-center gap-2 text-xs text-slate-500">
                    <span>Nodes drawn:</span>
                    {[100, 300, 1000, 3000].map((n) => (
                      <button key={n} onClick={() => setMaxNodes(n)} className={`rounded px-2 py-1 ${maxNodes === n ? "bg-blue-600 text-white" : "bg-slate-100 text-slate-700 hover:bg-slate-200"}`}>
                        {n}
                      </button>
                    ))}
                  </div>
                </div>
                {pathError && <p className="mt-2 text-sm text-red-700" role="alert">{pathError}</p>}
                {path && (
                  <div className="mt-4">
                    <p className="mb-2 text-sm">
                      {path.found ? <Badge tone="green">found</Badge> : <Badge tone="amber">not present</Badge>}{" "}
                      <span className="text-slate-700">Search for key <span className="font-mono">{path.key}</span> visited {path.node_ids.length} node(s).</span>
                    </p>
                    <DataTable columns={[{ label: "Step", align: "right" }, { label: "Node" }, { label: "Level", align: "right" }, { label: "Keys", align: "right" }, { label: "Decision" }]}>
                      {path.node_ids.map((id, i) => {
                        const n = byId.get(id);
                        let decision = "node not in the drawn portion of the tree";
                        if (n) {
                          if (n.leaf) decision = path.found ? `key ${path.key} is stored in this leaf` : `key ${path.key} would belong here but is absent`;
                          else {
                            const c = chosenChild(n, path.key);
                            const lo = c > 0 ? `${n.keys[c - 1]} ≤ key` : "";
                            const hi = c < n.keys.length ? `key < ${n.keys[c]}` : "";
                            decision = `follow child ${c + 1} of ${n.children.length}  (${[lo, hi].filter(Boolean).join(" and ")})`;
                          }
                        }
                        return (
                          <tr key={id} className="cursor-pointer hover:bg-slate-50" onClick={() => setSelectedId(id)}>
                            <td className="tabular px-3 py-1.5 text-right">{i + 1}</td>
                            <td className="px-3 py-1.5 font-mono">#{id} {n ? (n.leaf ? "(leaf)" : i === 0 ? "(root)" : "(internal)") : ""}</td>
                            <td className="tabular px-3 py-1.5 text-right">{n?.level ?? "—"}</td>
                            <td className="tabular px-3 py-1.5 text-right">{n?.keys.length ?? "—"}</td>
                            <td className="px-3 py-1.5 text-slate-700">{decision}</td>
                          </tr>
                        );
                      })}
                    </DataTable>
                  </div>
                )}
              </Card>

              <Card title="Tree" subtitle="Click a node for details. The bar under each node shows how full it is (amber = under half).">
                {t.truncated && (
                  <p className="mb-3 rounded-md bg-amber-50 px-3 py-2 text-sm text-amber-900" role="status">
                    Showing the first {formatInt(t.nodes_returned)} of {formatInt(t.node_count)} nodes (breadth-first), so the deepest levels are cut off. Raise “Nodes drawn” to see more.
                  </p>
                )}
                <BTreeView tree={t} pathIds={path?.node_ids ?? []} foundKey={path?.found ? path.key : null} selectedId={selectedId} onSelect={setSelectedId} />
                <div className="mt-3 flex flex-wrap gap-4 text-xs text-slate-600">
                  <span className="flex items-center gap-1.5"><span className="inline-block h-3 w-5 rounded border border-blue-400 bg-blue-50" /> internal node (separator keys)</span>
                  <span className="flex items-center gap-1.5"><span className="inline-block h-3 w-5 rounded border border-slate-400 bg-white" /> leaf node (keys → record locators)</span>
                  <span className="flex items-center gap-1.5"><span className="inline-block w-6 border-t-2 border-dashed border-slate-400" /> leaf sibling link</span>
                  <span className="flex items-center gap-1.5"><span className="inline-block w-6 border-t-[3px] border-blue-600" /> selected search path</span>
                </div>
              </Card>

              {selected && (
                <Card title={`Node #${selected.id}`} subtitle={selected.leaf ? "Leaf node" : selected.id === t.root_id ? "Root (internal) node" : "Internal node"}>
                  <dl className="grid grid-cols-2 gap-3 text-sm sm:grid-cols-4">
                    <div><dt className="text-xs text-slate-500">Level</dt><dd className="tabular font-medium">{selected.level}</dd></div>
                    <div><dt className="text-xs text-slate-500">Keys / capacity</dt><dd className="tabular font-medium">{selected.keys.length} / {t.max_keys} ({(selected.occupancy * 100).toFixed(0)}%)</dd></div>
                    <div><dt className="text-xs text-slate-500">Children</dt><dd className="tabular font-medium">{selected.leaf ? "— (leaf)" : selected.children.map((c) => `#${c}`).join(", ")}</dd></div>
                    <div><dt className="text-xs text-slate-500">Next leaf</dt><dd className="tabular font-medium">{selected.leaf ? (selected.next !== null ? `#${selected.next}` : "none (last leaf)") : "—"}</dd></div>
                  </dl>
                  <div className="mt-3 flex max-h-40 flex-wrap gap-1 overflow-y-auto">
                    {selected.keys.map((k) => (
                      <span key={k} className="rounded bg-slate-100 px-2 py-0.5 font-mono text-xs">{k}</span>
                    ))}
                  </div>
                </Card>
              )}
            </>
          )}
        </div>
      )}
    </>
  );
}
