import { useEffect, useMemo, useState } from "react";
import { Bar, BarChart, CartesianGrid, Legend, Tooltip, XAxis, YAxis } from "recharts";
import { ResponsiveContainer } from "recharts";
import { api, ApiError } from "../services/api";
import { useAsync } from "../hooks/useAsync";
import { useToast } from "../hooks/useToast";
import { Badge, Button, Card, DataTable, EmptyState, ErrorBanner, Field, LoadingBlock, PageHeader, inputClass } from "../components/ui";
import { formatInt, formatNs, formatThroughput, formatTime } from "../utils/format";
import type { BenchmarkParams, BenchmarkResults, JobState, JobStatus, SummaryRow } from "../types/api";

const SIZE_CHOICES = [10_000, 100_000, 1_000_000];
const ACTIVE: JobState[] = ["queued", "running"];

const tone = (s: JobState) => (s === "completed" ? "green" : s === "failed" ? "red" : s === "cancelled" ? "amber" : "blue");

function pick(rows: SummaryRow[], size: number, workload: string, path: string): number | null {
  return rows.find((r) => r.dataset_size === size && r.workload === workload && r.access_path === path)?.median_ns ?? null;
}

export default function PerformanceLab() {
  const toast = useToast();
  const [sizes, setSizes] = useState<number[]>([10_000]);
  const [reps, setReps] = useState(5);
  const [scanReps, setScanReps] = useState(5);
  const [rangeReps, setRangeReps] = useState(5);
  const [heavyReps, setHeavyReps] = useState(3);
  const [warmup, setWarmup] = useState(2);
  const [seed, setSeed] = useState(42);
  const [starting, setStarting] = useState(false);

  const jobs = useAsync(() => api.listJobs(), []);
  const [selected, setSelected] = useState<string | null>(null);
  const [job, setJob] = useState<JobStatus | null>(null);
  const [results, setResults] = useState<BenchmarkResults | null>(null);
  const [resErr, setResErr] = useState<string | null>(null);

  // Poll the selected job until it is finished.
  useEffect(() => {
    if (!selected) return;
    let stop = false;
    const tick = async () => {
      try {
        const j = await api.job(selected);
        if (stop) return;
        setJob(j);
        if (ACTIVE.includes(j.status)) {
          setTimeout(() => void tick(), 1000);
        } else {
          jobs.reload();
          if (j.results_available) {
            try {
              const r = await api.jobResults(selected);
              if (!stop) {
                setResults(r);
                setResErr(null);
              }
            } catch (e) {
              if (!stop) setResErr(e instanceof ApiError ? e.message : String(e));
            }
          }
        }
      } catch (e) {
        if (!stop) setResErr(e instanceof ApiError ? e.message : String(e));
      }
    };
    setResults(null);
    setResErr(null);
    void tick();
    return () => {
      stop = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selected]);

  async function start() {
    if (sizes.length === 0) {
      toast("error", "Choose at least one dataset size.");
      return;
    }
    setStarting(true);
    const params: BenchmarkParams = { sizes: [...sizes].sort((a, b) => a - b), reps, scan_reps: scanReps, range_reps: rangeReps, heavy_reps: heavyReps, warmup, max_keys: 64, seed };
    try {
      const j = await api.startBenchmark(params);
      toast("info", `Benchmark ${j.job_id} started.`);
      setSelected(j.job_id);
      jobs.reload();
    } catch (e) {
      toast("error", e instanceof ApiError ? e.message : String(e));
    } finally {
      setStarting(false);
    }
  }

  async function cancel() {
    if (!job) return;
    try {
      setJob(await api.cancelJob(job.job_id));
      toast("info", "Cancellation requested.");
    } catch (e) {
      toast("error", e instanceof ApiError ? e.message : String(e));
    }
  }

  const rows = results?.summary ?? [];
  const usedSizes = results?.meta.dataset_sizes ?? [];

  const pointData = useMemo(
    () =>
      usedSizes.map((n) => ({
        size: formatInt(n),
        "Full scan": pick(rows, n, "point_lookup", "FULL_SCAN"),
        "B+ Tree index": pick(rows, n, "point_lookup", "INDEX_POINT"),
      })),
    [rows, usedSizes],
  );
  const rangeSize = usedSizes.length ? usedSizes[usedSizes.length - 1] : 0;
  const rangeData = useMemo(
    () =>
      ["0.1pct", "1pct", "10pct", "50pct"].map((w) => ({
        selectivity: w.replace("pct", "%"),
        "Full scan": pick(rows, rangeSize, `range_${w}`, "FULL_SCAN"),
        "B+ Tree index": pick(rows, rangeSize, `range_${w}`, "INDEX_RANGE"),
      })),
    [rows, rangeSize],
  );
  const insertData = useMemo(
    () =>
      usedSizes.map((n) => ({
        size: formatInt(n),
        "Heap only": pick(rows, n, "insert_heap_only", "HEAP_ONLY"),
        "Index only": pick(rows, n, "insert_index_only", "INDEX_ONLY"),
        "Heap + index": pick(rows, n, "insert_full", "HEAP_PLUS_INDEX"),
      })),
    [rows, usedSizes],
  );

  const active = job && ACTIVE.includes(job.status);

  return (
    <>
      <PageHeader
        title="Performance Lab"
        subtitle="Runs the real indexcraft_bench executable against temporary data files. Every number shown below was measured by a run listed here; nothing is preloaded or sample data."
      />
      <div className="space-y-6">
        <Card title="Configure a run" subtitle="One benchmark at a time. Large sizes take minutes and a lot of disk space; the server enforces its own limits.">
          <div className="grid gap-4 md:grid-cols-3">
            <div>
              <div className="mb-1 text-xs font-medium text-slate-700">Dataset sizes</div>
              <div className="flex flex-wrap gap-3 text-sm">
                {SIZE_CHOICES.map((n) => (
                  <label key={n} className="flex items-center gap-1.5">
                    <input type="checkbox" checked={sizes.includes(n)} onChange={(e) => setSizes((s) => (e.target.checked ? [...s, n] : s.filter((x) => x !== n)))} />
                    {formatInt(n)}
                  </label>
                ))}
              </div>
            </div>
            <Field label="Point-lookup / insert repetitions"><input type="number" min={1} className={inputClass} value={reps} onChange={(e) => setReps(Number(e.target.value))} /></Field>
            <Field label="Full-scan repetitions"><input type="number" min={1} className={inputClass} value={scanReps} onChange={(e) => setScanReps(Number(e.target.value))} /></Field>
            <Field label="Range repetitions"><input type="number" min={1} className={inputClass} value={rangeReps} onChange={(e) => setRangeReps(Number(e.target.value))} /></Field>
            <Field label="Heavy-workload repetitions"><input type="number" min={1} className={inputClass} value={heavyReps} onChange={(e) => setHeavyReps(Number(e.target.value))} /></Field>
            <Field label="Warm-up runs"><input type="number" min={0} className={inputClass} value={warmup} onChange={(e) => setWarmup(Number(e.target.value))} /></Field>
            <Field label="Random seed"><input type="number" className={inputClass} value={seed} onChange={(e) => setSeed(Number(e.target.value))} /></Field>
          </div>
          <div className="mt-4 flex gap-2">
            <Button variant="primary" onClick={() => void start()} busy={starting} disabled={!!active}>Start benchmark</Button>
            {active && <Button variant="danger" onClick={() => void cancel()}>Cancel</Button>}
          </div>
        </Card>

        {job && (
          <Card title={`Job ${job.job_id}`} actions={<Badge tone={tone(job.status)}>{job.status}</Badge>}>
            <div className="mb-2 h-2 overflow-hidden rounded bg-slate-100">
              <div className="h-full bg-blue-600 transition-all" style={{ width: `${job.progress_percent}%` }} />
            </div>
            <div className="text-sm text-slate-600">
              {job.progress_percent.toFixed(0)}% · {job.stage || "—"}
              {job.current_size ? ` · dataset ${formatInt(job.current_size)}` : ""}
            </div>
            {job.error && <div className="mt-3"><ErrorBanner error={job.error} /></div>}
          </Card>
        )}

        {resErr && <ErrorBanner error={resErr} />}

        {results && (
          <>
            <Card
              title="Measured results"
              subtitle={`Measured in this run on ${results.meta.system.cpu_model} (${results.meta.system.logical_cpus} logical CPUs, ${formatInt(results.meta.system.ram_total_mib)} MiB RAM). Warm cache. Medians shown in charts.`}
              actions={
                <div className="flex gap-2">
                  <a className="text-sm font-medium text-blue-700 underline" href={api.csvUrl(results.job_id, "summary")}>summary.csv</a>
                  <a className="text-sm font-medium text-blue-700 underline" href={api.csvUrl(results.job_id, "raw")}>raw.csv ({formatInt(results.raw_rows)} rows)</a>
                </div>
              }
            >
              <div className="grid gap-6 lg:grid-cols-2">
                <Chart title="Point lookup latency (median, log scale)" data={pointData} x="size" keys={["Full scan", "B+ Tree index"]} />
                <Chart title={`Range query latency, ${formatInt(rangeSize)} records (median, log scale)`} data={rangeData} x="selectivity" keys={["Full scan", "B+ Tree index"]} />
                <Chart title="Insert cost per record (median, log scale)" data={insertData} x="size" keys={["Heap only", "Index only", "Heap + index"]} />
              </div>
            </Card>

            <Card title="Summary table">
              <DataTable columns={[{ label: "Size", align: "right" }, { label: "Workload" }, { label: "Path" }, { label: "Reps", align: "right" }, { label: "Mean", align: "right" }, { label: "Median", align: "right" }, { label: "p95", align: "right" }, { label: "p99", align: "right" }, { label: "Throughput", align: "right" }, { label: "Records examined", align: "right" }]}>
                {rows.map((r, i) => (
                  <tr key={i}>
                    <td className="tabular px-3 py-1.5 text-right">{formatInt(r.dataset_size)}</td>
                    <td className="px-3 py-1.5">{r.workload}</td>
                    <td className="px-3 py-1.5 font-mono text-xs">{r.access_path}</td>
                    <td className="tabular px-3 py-1.5 text-right">{r.access_path === "DERIVED" ? "—" : r.repetitions}</td>
                    <td className="tabular px-3 py-1.5 text-right">{formatNs(r.mean_ns)}</td>
                    <td className="tabular px-3 py-1.5 text-right">{r.access_path === "DERIVED" ? "—" : formatNs(r.median_ns)}</td>
                    <td className="tabular px-3 py-1.5 text-right">{r.access_path === "DERIVED" ? "—" : formatNs(r.p95_ns)}</td>
                    <td className="tabular px-3 py-1.5 text-right">{r.access_path === "DERIVED" ? "—" : formatNs(r.p99_ns)}</td>
                    <td className="tabular px-3 py-1.5 text-right">{r.access_path === "DERIVED" ? "—" : formatThroughput(r.throughput_ops_s)}</td>
                    <td className="tabular px-3 py-1.5 text-right">{formatInt(r.mean_records_examined)}</td>
                  </tr>
                ))}
              </DataTable>
              <p className="mt-2 text-xs text-slate-500">index_maintenance_overhead is derived (insert_full − insert_heap_only), not separately timed; a negative or near-zero value means the difference is within measurement noise. Percentiles with few repetitions are coarse.</p>
            </Card>

            <Card title="Methodology">
              <ul className="list-disc space-y-1 pl-5 text-sm text-slate-700">
                {Object.entries(results.meta.methodology).map(([k, v]) => (
                  <li key={k}><span className="font-medium">{k}:</span> {v}</li>
                ))}
                <li><span className="font-medium">build:</span> {results.meta.build.build_type}, {results.meta.build.compiler}, {results.meta.build.cxx_flags}</li>
                <li><span className="font-medium">OS:</span> {results.meta.system.os}</li>
              </ul>
            </Card>
          </>
        )}

        {!results && !job && <EmptyState title="No benchmark selected">Start a run above, or open a previous run below.</EmptyState>}

        <Card title="Previous runs (this API process)">
          {jobs.error && <ErrorBanner error={jobs.error} />}
          {!jobs.data && !jobs.error && <LoadingBlock />}
          {jobs.data && jobs.data.length === 0 && <p className="text-sm text-slate-500">None yet.</p>}
          {jobs.data && jobs.data.length > 0 && (
            <DataTable columns={[{ label: "Job" }, { label: "Status" }, { label: "Sizes" }, { label: "Created" }, { label: "" }]}>
              {jobs.data.map((j) => (
                <tr key={j.job_id}>
                  <td className="px-3 py-2 font-mono text-xs">{j.job_id}</td>
                  <td className="px-3 py-2"><Badge tone={tone(j.status)}>{j.status}</Badge></td>
                  <td className="px-3 py-2">{j.params.sizes.map(formatInt).join(", ")}</td>
                  <td className="px-3 py-2">{formatTime(j.created_at)}</td>
                  <td className="px-3 py-2 text-right"><Button variant="ghost" onClick={() => setSelected(j.job_id)}>View</Button></td>
                </tr>
              ))}
            </DataTable>
          )}
        </Card>
      </div>
    </>
  );
}

const COLORS = ["#dc2626", "#2563eb", "#059669"];

function Chart({ title, data, x, keys }: { title: string; data: Record<string, string | number | null>[]; x: string; keys: string[] }) {
  return (
    <div>
      <h3 className="mb-2 text-sm font-medium text-slate-800">{title}</h3>
      <div className="h-64">
        <ResponsiveContainer width="100%" height="100%">
          <BarChart data={data} margin={{ left: 10, right: 10 }}>
            <CartesianGrid strokeDasharray="3 3" />
            <XAxis dataKey={x} />
            <YAxis scale="log" domain={[(min: number) => Math.max(1, min / 2), (max: number) => max * 1.5]} tickFormatter={(v: number) => formatNs(v)} width={70} />
            <Tooltip formatter={(v) => formatNs(typeof v === "number" ? v : Number(v))} />
            <Legend />
            {keys.map((k, i) => (
              <Bar key={k} dataKey={k} fill={COLORS[i % COLORS.length]} />
            ))}
          </BarChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
}
