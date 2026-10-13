import { useState } from "react";
import { Link } from "react-router-dom";
import { api, ApiError } from "../services/api";
import { usePolling } from "../hooks/usePolling";
import { useToast } from "../hooks/useToast";
import { Badge, Button, Card, DataTable, EmptyState, ErrorBanner, LoadingBlock, PageHeader, StatCard } from "../components/ui";
import { formatBytes, formatInt, formatTime } from "../utils/format";

export default function Overview() {
  const stats = usePolling(() => api.stats(), 4000);
  const health = usePolling(() => api.health(), 4000);
  const ops = usePolling(() => api.operations(), 4000);
  const toast = useToast();
  const [busy, setBusy] = useState(false);

  async function generate() {
    setBusy(true);
    try {
      const r = await api.generateSamples(200);
      toast("success", `Inserted ${r.inserted} sample records (keys ${r.first_key}–${r.last_key}).`);
      const s = await api.stats();
      void s;
    } catch (e) {
      toast("error", e instanceof ApiError ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }

  const s = stats.data;
  const h = health.data;
  const ev = s?.index_events;

  return (
    <>
      <PageHeader
        title="Overview"
        subtitle="Live values read from the running C++ storage engine. Nothing on this page is simulated."
        actions={
          <Button variant="primary" onClick={generate} busy={busy} disabled={!h || h.status !== "ok"}>
            Generate 200 sample records
          </Button>
        }
      />
      {stats.error && !s && <ErrorBanner error={stats.error} />}
      {!s && !stats.error && <LoadingBlock label="Reading engine statistics…" />}
      {s && (
        <div className="space-y-6">
          <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-4">
            <StatCard label="Records" value={formatInt(s.record_count)} hint={`next record id ${formatInt(s.next_record_id)}`} />
            <StatCard label="Storage file" value={formatBytes(s.file_size_bytes)} hint={`${s.data_file} · ${formatInt(s.deleted_slots)} reusable deleted slots`} />
            <StatCard label="Index height" value={s.index_height} hint={`node capacity ${s.index_max_keys} keys`} />
            <StatCard label="Indexed keys" value={formatInt(s.index_key_count)} hint={s.index_key_count === s.record_count ? "matches record count" : "differs from record count!"} tone={s.index_key_count === s.record_count ? "good" : "bad"} />
            <StatCard label="Index nodes" value={formatInt(s.index_node_count)} hint={`${formatInt(s.index_leaf_count)} leaf · ${formatInt(s.index_internal_count)} internal`} />
            <StatCard label="Engine health" value={h?.status === "ok" ? "Healthy" : "Degraded"} tone={h?.status === "ok" ? "good" : "bad"} hint={h?.status === "ok" ? "heap file open, index built" : h?.error ?? ""} />
            <StatCard label="Index rebuilt at startup" value={`${s.open_rebuild_ms.toFixed(2)} ms`} hint="file validation + B+ Tree rebuild" />
            <StatCard label="Index persistence" value="Rebuilt" hint="only the heap file is persisted" />
          </div>

          {s.record_count === 0 && (
            <EmptyState
              title="The store is empty"
              action={<Button variant="primary" onClick={generate} busy={busy}>Generate 200 sample records</Button>}
            >
              Insert records in the <Link className="text-blue-700 underline" to="/storage">Storage Explorer</Link> or run SQL in the{" "}
              <Link className="text-blue-700 underline" to="/query">Query Workbench</Link>. Sample records are real inserts through the engine.
            </EmptyState>
          )}

          <div className="grid gap-6 lg:grid-cols-2">
            <Card title="B+ Tree structural events" subtitle="Counted by the engine since the index was last built">
              {ev && (
                <DataTable columns={[{ label: "Event" }, { label: "Count", align: "right" }]}>
                  {(
                    [
                      ["Leaf splits", ev.leaf_splits],
                      ["Internal-node splits", ev.internal_splits],
                      ["Root splits (height grew)", ev.root_splits],
                      ["Leaf redistributions", ev.leaf_redistributions],
                      ["Internal redistributions", ev.internal_redistributions],
                      ["Leaf merges", ev.leaf_merges],
                      ["Internal merges", ev.internal_merges],
                      ["Root shrinks (height shrank)", ev.root_shrinks],
                    ] as [string, number][]
                  ).map(([name, n]) => (
                    <tr key={name}>
                      <td className="px-3 py-2 text-slate-700">{name}</td>
                      <td className="tabular px-3 py-2 text-right font-medium">{formatInt(n)}</td>
                    </tr>
                  ))}
                </DataTable>
              )}
            </Card>

            <Card title="Recent operations" subtitle="Handled by the API since it started">
              {!ops.data && !ops.error && <LoadingBlock />}
              {ops.data && ops.data.operations.length === 0 && (
                <EmptyState title="No operations yet">Inserts, deletes and queries you run will be listed here.</EmptyState>
              )}
              {ops.data && ops.data.operations.length > 0 && (
                <ul className="max-h-80 divide-y divide-slate-100 overflow-y-auto text-sm">
                  {ops.data.operations.slice(0, 15).map((o, i) => (
                    <li key={`${o.time}-${i}`} className="flex items-center gap-3 py-2">
                      <Badge tone={o.ok ? "green" : "red"}>{o.operation}</Badge>
                      <span className="min-w-0 flex-1 truncate font-mono text-xs text-slate-700" title={o.detail}>{o.detail}</span>
                      <span className="shrink-0 text-xs text-slate-400">{o.duration_ms !== null ? `${o.duration_ms.toFixed(3)} ms · ` : ""}{formatTime(o.time).split(", ").pop()}</span>
                    </li>
                  ))}
                </ul>
              )}
            </Card>
          </div>
        </div>
      )}
    </>
  );
}
