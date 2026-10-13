import type { QueryResult } from "../types/api";
import { formatInt, formatNs } from "../utils/format";
import { PathBadge } from "./ui";

export function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="rounded-lg bg-slate-50 px-3 py-2">
      <div className="text-[11px] font-medium uppercase tracking-wide text-slate-500">{label}</div>
      <div className="tabular text-sm font-semibold text-slate-900">{value}</div>
    </div>
  );
}

export function ResultMetrics({ r }: { r: QueryResult }) {
  return (
    <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-6">
      <Metric label="Execution time" value={formatNs(r.exec_ns)} />
      <Metric label="Parse time" value={formatNs(r.parse_ns)} />
      <Metric label="Records examined" value={formatInt(r.records_examined)} />
      <Metric label="Index lookups" value={formatInt(r.index_lookups)} />
      <Metric label="Index node visits" value={formatInt(r.index_nodes_visited)} />
      <Metric label="Heap read calls" value={formatInt(r.heap_reads)} />
    </div>
  );
}

export default function PlanPanel({ result }: { result: QueryResult }) {
  return (
    <div>
      <div className="mb-3 flex flex-wrap items-center gap-2">
        <span className="text-xs font-medium uppercase tracking-wide text-slate-500">Access path</span>
        <PathBadge path={result.access_path} forced={result.path_forced} />
        {!result.executed && <span className="text-xs text-slate-500">(planned only — not executed)</span>}
      </div>
      <ol className="space-y-1.5 text-sm text-slate-700">
        {result.plan.map((line, i) => (
          <li key={i} className={`rounded-md px-3 py-1.5 ${line.startsWith("Actual") ? "bg-blue-50 text-blue-900" : line.startsWith("Access path") ? "bg-slate-100 font-medium" : "bg-slate-50"}`}>
            {line}
          </li>
        ))}
      </ol>
    </div>
  );
}
