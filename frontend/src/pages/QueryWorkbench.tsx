import { useState } from "react";
import { api, ApiError } from "../services/api";
import { useToast } from "../hooks/useToast";
import type { CompareResult, DbRecord, QueryResult } from "../types/api";
import { Badge, Button, Card, DataTable, EmptyState, ErrorBanner, Field, PageHeader, PathBadge, inputClass } from "../components/ui";
import PlanPanel, { Metric, ResultMetrics } from "../components/PlanPanel";
import { formatInt, formatNs } from "../utils/format";

const EXAMPLES: { label: string; sql: string }[] = [
  { label: "Insert a record", sql: "INSERT INTO records (key, payload) VALUES (1001, 'hello from the workbench');" },
  { label: "Point lookup", sql: "SELECT * FROM records WHERE key = 100;" },
  { label: "Range query", sql: "SELECT * FROM records WHERE key >= 50 AND key <= 80;" },
  { label: "Range with LIMIT", sql: "SELECT * FROM records WHERE key >= 1 AND key <= 150 LIMIT 10;" },
  { label: "BETWEEN", sql: "SELECT * FROM records WHERE key BETWEEN 20 AND 30;" },
  { label: "Full table", sql: "SELECT * FROM records LIMIT 20;" },
  { label: "Delete", sql: "DELETE FROM records WHERE key = 1001;" },
  { label: "Explain", sql: "EXPLAIN SELECT * FROM records WHERE key >= 10 AND key <= 20;" },
];

type PathChoice = "auto" | "scan" | "index";

function RowsTable({ rows, hasMore }: { rows: DbRecord[]; hasMore: boolean }) {
  if (rows.length === 0) return <EmptyState title="No rows matched" />;
  return (
    <>
      <DataTable columns={[{ label: "key", align: "right" }, { label: "record id", align: "right" }, { label: "payload" }]} className="max-h-96 overflow-y-auto">
        {rows.map((r) => (
          <tr key={r.rid} className="hover:bg-slate-50">
            <td className="tabular px-3 py-1.5 text-right font-mono">{r.key}</td>
            <td className="tabular px-3 py-1.5 text-right font-mono text-slate-500">{r.rid}</td>
            <td className="max-w-xl truncate px-3 py-1.5 font-mono text-xs" title={r.payload}>{r.payload}</td>
          </tr>
        ))}
      </DataTable>
      {hasMore && <p className="mt-2 text-xs text-amber-700">More rows match than are shown (result capped by the row limit).</p>}
    </>
  );
}

function CompareView({ c }: { c: CompareResult }) {
  const max = Math.max(c.scan.exec_ns, c.index.exec_ns, 1);
  const bar = (r: QueryResult, color: string) => (
    <div>
      <div className="mb-1 flex items-center justify-between text-sm">
        <span className="flex items-center gap-2"><PathBadge path={r.access_path} forced /> <span className="text-slate-600">{formatInt(r.records_examined)} records examined</span></span>
        <span className="tabular font-semibold">{formatNs(r.exec_ns)}</span>
      </div>
      <div className="h-3 w-full rounded bg-slate-100"><div className={`h-3 rounded ${color}`} style={{ width: `${Math.max((r.exec_ns / max) * 100, 1.5)}%` }} /></div>
    </div>
  );
  return (
    <div className="space-y-4">
      <div className={`rounded-lg border px-4 py-3 text-sm ${c.results_match ? "border-emerald-200 bg-emerald-50 text-emerald-900" : "border-red-200 bg-red-50 text-red-900"}`} role="status">
        {c.results_match ? (
          <>
            <strong>Results identical.</strong> Both paths returned the same {c.scan.rows.length} row(s) (keys, record ids and payloads
            compared).
          </>
        ) : (
          <>
            <strong>Results differ!</strong> {c.mismatch}
          </>
        )}
      </div>
      <div className="space-y-3">
        {bar(c.scan, "bg-amber-500")}
        {bar(c.index, "bg-blue-600")}
      </div>
      <div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
        <Metric label="Index speed-up" value={`${c.speedup.toFixed(1)}×`} />
        <Metric label="Rows returned" value={formatInt(c.scan.rows.length)} />
        <Metric label="Index node visits" value={formatInt(c.index.index_nodes_visited)} />
        <Metric label="Heap read calls (index)" value={formatInt(c.index.heap_reads)} />
      </div>
      <p className="text-xs text-slate-500">Each path ran once, back to back, against the warm OS page cache. Single runs are noisy; use the Performance Lab for statistics.</p>
    </div>
  );
}

export default function QueryWorkbench() {
  const toast = useToast();
  const [sql, setSql] = useState(EXAMPLES[2].sql);
  const [path, setPath] = useState<PathChoice>("auto");
  const [maxRows, setMaxRows] = useState(100);
  const [busy, setBusy] = useState<"run" | "compare" | null>(null);
  const [result, setResult] = useState<QueryResult | null>(null);
  const [compare, setCompare] = useState<CompareResult | null>(null);
  const [error, setError] = useState<ApiError | null>(null);

  async function run(text = sql) {
    setBusy("run");
    setError(null);
    setCompare(null);
    try {
      const r = await api.query(text, path, maxRows);
      setResult(r);
      if (r.statement === "INSERT" || r.statement === "DELETE") toast("success", r.message);
    } catch (e) {
      setResult(null);
      setError(e instanceof ApiError ? e : new ApiError(0, "ERROR", String(e)));
    } finally {
      setBusy(null);
    }
  }

  async function runCompare() {
    setBusy("compare");
    setError(null);
    setResult(null);
    try {
      setCompare(await api.compare(sql, maxRows));
    } catch (e) {
      setCompare(null);
      setError(e instanceof ApiError ? e : new ApiError(0, "ERROR", String(e)));
    } finally {
      setBusy(null);
    }
  }

  return (
    <>
      <PageHeader
        title="Query Workbench"
        subtitle="Run the SQL-like subset against the engine. SELECT results are always ordered by key; every run reports the access path, records examined and engine-measured time."
      />
      <div className="space-y-6">
        <Card>
          <div className="mb-3 flex flex-wrap gap-2">
            {EXAMPLES.map((e) => (
              <button key={e.label} onClick={() => setSql(e.sql)} className="rounded-full border border-slate-300 bg-white px-3 py-1 text-xs font-medium text-slate-700 hover:border-blue-400 hover:text-blue-700">
                {e.label}
              </button>
            ))}
          </div>
          <label className="sr-only" htmlFor="sql">SQL query</label>
          <textarea
            id="sql"
            value={sql}
            onChange={(e) => setSql(e.target.value)}
            onKeyDown={(e) => {
              if ((e.ctrlKey || e.metaKey) && e.key === "Enter") void run();
            }}
            spellCheck={false}
            rows={4}
            className={`${inputClass} font-mono leading-relaxed`}
            placeholder="SELECT * FROM records WHERE key = 1;"
          />
          <div className="mt-3 flex flex-wrap items-end gap-4">
            <Field label="Access path (SELECT only)">
              <select value={path} onChange={(e) => setPath(e.target.value as PathChoice)} className={`${inputClass} w-56`}>
                <option value="auto">Automatic (planner chooses)</option>
                <option value="scan">Force full table scan</option>
                <option value="index">Force B+ Tree index</option>
              </select>
            </Field>
            <Field label="Max rows shown">
              <input type="number" min={1} max={5000} value={maxRows} onChange={(e) => setMaxRows(Math.min(5000, Math.max(1, Number(e.target.value) || 1)))} className={`${inputClass} w-28`} />
            </Field>
            <div className="ml-auto flex flex-wrap gap-2">
              <Button onClick={() => void run(`EXPLAIN ${sql.replace(/^\s*EXPLAIN\s+/i, "")}`)} disabled={!!busy || !sql.trim()}>
                Explain
              </Button>
              <Button onClick={() => void runCompare()} busy={busy === "compare"} disabled={!!busy || !sql.trim()} title="Runs the same SELECT through a full scan and the index and checks the rows match">
                Compare scan vs index
              </Button>
              <Button variant="primary" onClick={() => void run()} busy={busy === "run"} disabled={!!busy || !sql.trim()}>
                Run <span className="hidden text-xs opacity-70 sm:inline">Ctrl+Enter</span>
              </Button>
            </div>
          </div>
        </Card>

        {error && (
          <div>
            <ErrorBanner error={error} />
            {error.code === "SYNTAX_ERROR" || error.code === "UNSUPPORTED" ? (
              <p className="mt-2 text-xs text-slate-500">See the supported syntax under Diagnostics → Supported query syntax.</p>
            ) : null}
          </div>
        )}

        {!result && !compare && !error && (
          <EmptyState title="No query run yet">Pick an example above or write your own, then press Run. The insert/delete examples change the real data file.</EmptyState>
        )}

        {result && (
          <div className="space-y-6">
            <Card title="Result" subtitle={result.message} actions={<div className="flex items-center gap-2"><PathBadge path={result.access_path} forced={result.path_forced} />{result.statement && <Badge>{result.statement}</Badge>}</div>}>
              {result.executed ? <ResultMetrics r={result} /> : <p className="text-sm text-slate-600">EXPLAIN only plans the statement; nothing was executed.</p>}
              {result.statement === "SELECT" && result.executed && (
                <div className="mt-4">
                  <RowsTable rows={result.rows} hasMore={result.has_more} />
                </div>
              )}
              {(result.statement === "INSERT" || result.statement === "DELETE") && result.executed && (
                <p className="mt-4 text-sm text-slate-700">{result.rows_affected} row(s) affected.</p>
              )}
            </Card>
            <Card title="Execution plan" subtitle="Produced by the engine's planner; the last line is measured from this run">
              <PlanPanel result={result} />
            </Card>
          </div>
        )}

        {compare && (
          <div className="space-y-6">
            <Card title="Scan versus index" subtitle="Same logical query, two real execution paths">
              <CompareView c={compare} />
            </Card>
            <div className="grid gap-6 lg:grid-cols-2">
              <Card title="Full scan plan"><PlanPanel result={compare.scan} /></Card>
              <Card title="Index plan"><PlanPanel result={compare.index} /></Card>
            </div>
            <Card title="Rows returned"><RowsTable rows={compare.index.rows} hasMore={compare.index.has_more} /></Card>
          </div>
        )}
      </div>
    </>
  );
}
