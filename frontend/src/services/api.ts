import type {
  BenchmarkParams,
  BenchmarkResults,
  BuildInfo,
  CompareResult,
  Consistency,
  DbRecord,
  EngineErrorEntry,
  Health,
  InsertResult,
  JobStatus,
  OperationEntry,
  QueryResult,
  RecordList,
  SearchPath,
  Stats,
  SyntaxDoc,
  TestResults,
  TreeData,
  TreeValidation,
} from "../types/api";

export class ApiError extends Error {
  status: number;
  code: string;
  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

function describeDetail(detail: unknown): { code: string; message: string } {
  if (detail && typeof detail === "object" && !Array.isArray(detail)) {
    const d = detail as { code?: string; message?: string };
    if (d.message) return { code: d.code ?? "ERROR", message: d.message };
  }
  if (Array.isArray(detail)) {
    // FastAPI/pydantic validation errors
    const msg = detail
      .map((e: { loc?: (string | number)[]; msg?: string }) => `${(e.loc ?? []).filter((p) => p !== "body").join(".")}: ${e.msg ?? "invalid"}`)
      .join("; ");
    return { code: "VALIDATION_ERROR", message: msg };
  }
  if (typeof detail === "string") return { code: "ERROR", message: detail };
  return { code: "ERROR", message: "Request failed" };
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`/api${path}`, {
      ...init,
      headers: { "Content-Type": "application/json", ...(init?.headers ?? {}) },
    });
  } catch {
    throw new ApiError(0, "NETWORK", "Cannot reach the IndexCraft API. Is the backend running on port 8000?");
  }
  if (!res.ok) {
    let body: { detail?: unknown } = {};
    try {
      body = await res.json();
    } catch {
      /* non-JSON error body */
    }
    const { code, message } = describeDetail(body.detail);
    throw new ApiError(res.status, code, message);
  }
  return (await res.json()) as T;
}

const json = (body: unknown): RequestInit => ({ method: "POST", body: JSON.stringify(body) });

export const api = {
  health: () => request<Health>("/health"),
  stats: () => request<Stats>("/stats"),

  listRecords: (offset: number, limit: number) => request<RecordList>(`/records?offset=${offset}&limit=${limit}`),
  getRecord: (key: number) => request<DbRecord>(`/records/${key}`),
  insertRecord: (key: number, payload: string) => request<InsertResult>("/records", json({ key, payload })),
  deleteRecord: (key: number) => request<{ key: number; deleted: boolean }>(`/records/${key}`, { method: "DELETE" }),
  generateSamples: (count: number) =>
    request<{ inserted: number; skipped_duplicates: number; first_key: number; last_key: number }>(
      "/records/sample",
      json({ count }),
    ),

  query: (sql: string, forcePath: "auto" | "scan" | "index", maxRows: number) =>
    request<QueryResult>("/query", json({ sql, force_path: forcePath, max_rows: maxRows })),
  compare: (sql: string, maxRows: number) => request<CompareResult>("/query/compare", json({ sql, max_rows: maxRows })),

  tree: (maxNodes: number) => request<TreeData>(`/index/tree?max_nodes=${maxNodes}`),
  searchPath: (key: number) => request<SearchPath>(`/index/path/${key}`),
  validateTree: () => request<TreeValidation>("/index/validate"),
  consistency: () => request<Consistency>("/index/consistency"),
  rebuildIndex: () => request<{ index_key_count: number }>("/index/rebuild", { method: "POST" }),

  startBenchmark: (p: BenchmarkParams) => request<JobStatus>("/benchmark", json(p)),
  listJobs: () => request<JobStatus[]>("/benchmark"),
  job: (id: string) => request<JobStatus>(`/benchmark/${id}`),
  cancelJob: (id: string) => request<JobStatus>(`/benchmark/${id}`, { method: "DELETE" }),
  jobResults: (id: string) => request<BenchmarkResults>(`/benchmark/${id}/results`),
  csvUrl: (id: string, kind: "raw" | "summary") => `/api/benchmark/${id}/csv/${kind}`,

  buildInfo: () => request<BuildInfo>("/diagnostics/build"),
  syntax: () => request<SyntaxDoc>("/diagnostics/syntax"),
  errors: () => request<{ errors: EngineErrorEntry[] }>("/diagnostics/errors"),
  operations: () => request<{ operations: OperationEntry[] }>("/diagnostics/operations"),
  testResults: () => request<TestResults>("/diagnostics/test-results"),
};
