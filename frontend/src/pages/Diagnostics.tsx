import { useState } from "react";
import { api, ApiError } from "../services/api";
import { useAsync } from "../hooks/useAsync";
import { useToast } from "../hooks/useToast";
import { Badge, Button, Card, DataTable, EmptyState, ErrorBanner, LoadingBlock, PageHeader } from "../components/ui";
import { formatTime } from "../utils/format";

export default function Diagnostics() {
  const toast = useToast();
  const consistency = useAsync(() => api.consistency(), []);
  const validation = useAsync(() => api.validateTree(), []);
  const tests = useAsync(() => api.testResults(), []);
  const syntax = useAsync(() => api.syntax(), []);
  const build = useAsync(() => api.buildInfo(), []);
  const errors = useAsync(() => api.errors(), []);
  const [busy, setBusy] = useState(false);

  const recheck = () => {
    consistency.reload();
    validation.reload();
    errors.reload();
  };

  async function rebuild() {
    setBusy(true);
    try {
      const r = await api.rebuildIndex();
      toast("success", `Index rebuilt from heap: ${r.index_key_count} keys.`);
      recheck();
    } catch (e) {
      toast("error", e instanceof ApiError ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }

  const c = consistency.data;
  const v = validation.data;

  return (
    <>
      <PageHeader
        title="Diagnostics"
        subtitle="Integrity checks run on demand against the live engine; test results come from the last recorded test run."
        actions={
          <>
            <Button onClick={recheck}>Re-run checks</Button>
            <Button variant="danger" busy={busy} onClick={() => void rebuild()}>
              Rebuild index from heap
            </Button>
          </>
        }
      />
      <div className="space-y-6">
        <div className="grid gap-6 lg:grid-cols-2">
          <Card title="Heap ↔ index consistency" subtitle="Every live heap record must have an index entry pointing at it, and vice versa.">
            {consistency.error && <ErrorBanner error={consistency.error} />}
            {!c && !consistency.error && <LoadingBlock />}
            {c && (
              <div className="space-y-2 text-sm">
                <Badge tone={c.consistent ? "green" : "red"}>{c.consistent ? "Consistent" : "Inconsistent"}</Badge>
                <div className="tabular text-slate-700">
                  heap live records {c.heap_live_records} · index entries {c.index_entries} · checked {c.records_checked}
                </div>
                {c.problems.length > 0 && (
                  <ul className="list-disc pl-5 text-red-800">
                    {c.problems.slice(0, 20).map((p, i) => (
                      <li key={i}>{p}</li>
                    ))}
                  </ul>
                )}
              </div>
            )}
          </Card>
          <Card title="B+ Tree invariants" subtitle="Key order, separator bounds, uniform leaf depth, leaf chain, key counts.">
            {validation.error && <ErrorBanner error={validation.error} />}
            {!v && !validation.error && <LoadingBlock />}
            {v && (
              <div className="space-y-2 text-sm">
                <Badge tone={v.valid ? "green" : "red"}>{v.valid ? "Valid" : "Violations found"}</Badge>
                <div className="tabular text-slate-700">
                  height {v.height} · {v.node_count} nodes · {v.key_count} keys
                </div>
                {v.violations.length > 0 && (
                  <ul className="list-disc pl-5 text-red-800">
                    {v.violations.slice(0, 20).map((p, i) => (
                      <li key={i}>{p}</li>
                    ))}
                  </ul>
                )}
              </div>
            )}
          </Card>
        </div>

        <Card title="Recorded test results" subtitle="Read from docs/test-results/latest.json, written by scripts/run_all_tests.sh. Not computed in the browser.">
          {tests.error && <ErrorBanner error={tests.error} />}
          {!tests.data && !tests.error && <LoadingBlock />}
          {tests.data && !tests.data.available && <EmptyState title="No test results recorded yet">{tests.data.message}</EmptyState>}
          {tests.data && tests.data.available && (
            <>
              <p className="mb-3 text-xs text-slate-500">Generated {formatTime(tests.data.generated_at)}</p>
              <DataTable columns={[{ label: "Suite" }, { label: "Framework" }, { label: "Total", align: "right" }, { label: "Passed", align: "right" }, { label: "Failed", align: "right" }, { label: "Skipped", align: "right" }, { label: "Seconds", align: "right" }]}>
                {tests.data.suites.map((s) => (
                  <tr key={s.name}>
                    <td className="px-3 py-2">{s.name}</td>
                    <td className="px-3 py-2">{s.framework}</td>
                    <td className="tabular px-3 py-2 text-right">{s.total}</td>
                    <td className="tabular px-3 py-2 text-right text-emerald-700">{s.passed}</td>
                    <td className={`tabular px-3 py-2 text-right ${s.failed ? "font-semibold text-red-700" : ""}`}>{s.failed}</td>
                    <td className="tabular px-3 py-2 text-right">{s.skipped}</td>
                    <td className="tabular px-3 py-2 text-right">{s.duration_seconds.toFixed(1)}</td>
                  </tr>
                ))}
              </DataTable>
            </>
          )}
        </Card>

        <div className="grid gap-6 lg:grid-cols-2">
          <Card title="Supported query syntax">
            {syntax.error && <ErrorBanner error={syntax.error} />}
            {!syntax.data && !syntax.error && <LoadingBlock />}
            {syntax.data && (
              <div className="space-y-3 text-sm">
                {syntax.data.statements.map((s) => (
                  <div key={s.name}>
                    <div className="font-medium">{s.name}</div>
                    <code className="block rounded bg-slate-100 px-2 py-1 font-mono text-xs">{s.syntax}</code>
                    <div className="text-xs text-slate-500">{s.notes}</div>
                  </div>
                ))}
                <ul className="list-disc pl-5 text-xs text-slate-600">
                  {syntax.data.rules.map((r, i) => (
                    <li key={i}>{r}</li>
                  ))}
                </ul>
              </div>
            )}
          </Card>
          <Card title="Build information">
            {build.error && <ErrorBanner error={build.error} />}
            {!build.data && !build.error && <LoadingBlock />}
            {build.data && (
              <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
                {build.data.engine ? (
                  <>
                    <dt className="text-slate-500">Engine version</dt><dd>{build.data.engine.version}</dd>
                    <dt className="text-slate-500">Build type</dt><dd>{build.data.engine.build_type}</dd>
                    <dt className="text-slate-500">Compiler</dt><dd className="break-all">{build.data.engine.compiler}</dd>
                    <dt className="text-slate-500">CXX flags</dt><dd className="break-all font-mono text-xs">{build.data.engine.cxx_flags}</dd>
                    <dt className="text-slate-500">C++ standard</dt><dd>{build.data.engine.cxx_standard}</dd>
                    <dt className="text-slate-500">Storage format</dt><dd>v{build.data.engine.storage_format_version}</dd>
                    <dt className="text-slate-500">Max payload</dt><dd>{build.data.engine.max_payload_bytes} bytes</dd>
                    <dt className="text-slate-500">Scan threshold</dt><dd>{build.data.engine.scan_selectivity_threshold}</dd>
                  </>
                ) : (
                  <dd className="col-span-2 text-red-800">Engine unavailable: {build.data.engine_error}</dd>
                )}
                <dt className="text-slate-500">Python</dt><dd>{build.data.backend.python}</dd>
                <dt className="text-slate-500">Platform</dt><dd className="break-all">{build.data.backend.platform}</dd>
              </dl>
            )}
          </Card>
        </div>

        <Card title="Recent engine errors" subtitle="Rejected operations (duplicate keys, parse errors, …) since the API started.">
          {errors.error && <ErrorBanner error={errors.error} />}
          {errors.data && errors.data.errors.length === 0 && <p className="text-sm text-slate-500">No errors recorded.</p>}
          {errors.data && errors.data.errors.length > 0 && (
            <DataTable columns={[{ label: "Time" }, { label: "Operation" }, { label: "Code" }, { label: "Message" }]}>
              {errors.data.errors.slice().reverse().map((e, i) => (
                <tr key={i}>
                  <td className="px-3 py-2 whitespace-nowrap">{formatTime(e.time)}</td>
                  <td className="px-3 py-2">{e.operation}</td>
                  <td className="px-3 py-2 font-mono text-xs">{e.code}</td>
                  <td className="px-3 py-2">{e.message}</td>
                </tr>
              ))}
            </DataTable>
          )}
        </Card>
      </div>
    </>
  );
}
