// Types mirroring the FastAPI response models (backend/app/schemas.py).

export interface Health {
  status: "ok" | "degraded";
  engine_loaded: boolean;
  engine_open: boolean;
  data_file: string;
  error: string | null;
  api_version: string;
  uptime_seconds: number;
}

export interface IndexEvents {
  leaf_splits: number;
  internal_splits: number;
  root_splits: number;
  leaf_redistributions: number;
  internal_redistributions: number;
  leaf_merges: number;
  internal_merges: number;
  root_shrinks: number;
}

export interface Stats {
  data_file: string;
  record_count: number;
  file_size_bytes: number;
  deleted_slots: number;
  next_record_id: number;
  index_height: number;
  index_key_count: number;
  index_node_count: number;
  index_leaf_count: number;
  index_internal_count: number;
  index_max_keys: number;
  open_rebuild_ms: number;
  index_events: IndexEvents;
}

export interface DbRecord {
  key: number;
  rid: number;
  payload: string;
}

export interface RecordList {
  total: number;
  offset: number;
  limit: number;
  records: DbRecord[];
}

export interface InsertResult {
  key: number;
  rid: number;
  offset: number;
}

export type AccessPath = "NONE" | "FULL_SCAN" | "INDEX_POINT" | "INDEX_RANGE" | "INDEX_INSERT";

export interface QueryResult {
  ok: boolean;
  statement: string | null;
  access_path: AccessPath;
  path_forced: boolean;
  executed: boolean;
  rows: DbRecord[];
  row_count: number;
  rows_matched: number;
  has_more: boolean;
  rows_affected: number;
  records_examined: number;
  index_lookups: number;
  index_nodes_visited: number;
  heap_reads: number;
  parse_ns: number;
  exec_ns: number;
  total_ns: number;
  duration_ms: number;
  plan: string[];
  message: string;
}

export interface CompareResult {
  ok: boolean;
  results_match: boolean;
  mismatch: string;
  speedup: number;
  scan: QueryResult;
  index: QueryResult;
}

export interface TreeNode {
  id: number;
  leaf: boolean;
  level: number;
  keys: number[];
  children: number[];
  next: number | null;
  occupancy: number;
}

export interface TreeData {
  max_keys: number;
  height: number;
  key_count: number;
  node_count: number;
  leaf_count: number;
  internal_count: number;
  root_id: number;
  nodes_returned: number;
  truncated: boolean;
  nodes: TreeNode[];
}

export interface SearchPath {
  key: number;
  found: boolean;
  node_ids: number[];
}

export interface TreeValidation {
  valid: boolean;
  violations: string[];
  height: number;
  key_count: number;
  node_count: number;
}

export interface Consistency {
  consistent: boolean;
  heap_live_records: number;
  index_entries: number;
  records_checked: number;
  problems: string[];
}

export type JobState = "queued" | "running" | "completed" | "failed" | "cancelled";

export interface BenchmarkParams {
  sizes: number[];
  reps: number;
  scan_reps: number;
  range_reps: number;
  heavy_reps: number;
  warmup: number;
  max_keys: number;
  seed: number;
}

export interface JobStatus {
  job_id: string;
  status: JobState;
  created_at: string;
  started_at: string | null;
  finished_at: string | null;
  progress_percent: number;
  stage: string;
  current_size: number | null;
  params: BenchmarkParams;
  error: string | null;
  results_available: boolean;
}

export interface SummaryRow {
  run_id: string;
  dataset_size: number;
  workload: string;
  access_path: string;
  repetitions: number;
  mean_ns: number | null;
  median_ns: number | null;
  p90_ns: number | null;
  p95_ns: number | null;
  p99_ns: number | null;
  min_ns: number | null;
  max_ns: number | null;
  stddev_ns: number | null;
  throughput_ops_s: number | null;
  mean_records_examined: number | null;
  notes: string;
}

export interface BenchmarkMeta {
  run_id: string;
  started_utc: string;
  finished_utc: string;
  dataset_sizes: number[];
  parameters: Record<string, number>;
  system: { cpu_model: string; logical_cpus: number; ram_total_mib: number; os: string };
  build: { version: string; build_type: string; compiler: string; cxx_flags: string };
  methodology: Record<string, string>;
}

export interface BenchmarkResults {
  job_id: string;
  measured: boolean;
  meta: BenchmarkMeta;
  summary: SummaryRow[];
  raw_rows: number;
}

export interface BuildInfo {
  backend: { python: string; platform: string; max_benchmark_size?: number };
  engine: {
    version: string;
    build_type: string;
    compiler: string;
    cxx_flags: string;
    cxx_standard: number;
    storage_format_version: number;
    max_payload_bytes: number;
    scan_selectivity_threshold: number;
  } | null;
  engine_error: string | null;
}

export interface SyntaxDoc {
  statements: { name: string; syntax: string; notes: string }[];
  rules: string[];
}

export interface OperationEntry {
  time: string;
  operation: string;
  detail: string;
  ok: boolean;
  duration_ms: number | null;
}

export interface EngineErrorEntry {
  time: string;
  operation: string;
  code: string;
  message: string;
}

export interface TestSuite {
  name: string;
  framework: string;
  total: number;
  passed: number;
  failed: number;
  skipped: number;
  duration_seconds: number;
  failures: string[];
}

export type TestResults =
  | { available: false; message: string }
  | { available: true; generated_at: string; suites: TestSuite[] };
