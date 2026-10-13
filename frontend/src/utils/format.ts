export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  const units = ["KiB", "MiB", "GiB", "TiB"];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(v >= 100 ? 0 : v >= 10 ? 1 : 2)} ${units[i]}`;
}

/** Nanoseconds -> a readable duration (ns, µs, ms, s). */
export function formatNs(ns: number | null | undefined): string {
  if (ns === null || ns === undefined || Number.isNaN(ns)) return "—";
  const abs = Math.abs(ns);
  if (abs < 1_000) return `${ns.toFixed(0)} ns`;
  if (abs < 1_000_000) return `${(ns / 1_000).toFixed(abs < 10_000 ? 2 : 1)} µs`;
  if (abs < 1_000_000_000) return `${(ns / 1_000_000).toFixed(abs < 10_000_000 ? 2 : 1)} ms`;
  return `${(ns / 1_000_000_000).toFixed(2)} s`;
}

export const formatInt = (n: number | null | undefined) =>
  n === null || n === undefined ? "—" : Math.round(n).toLocaleString("en-US");

export function formatThroughput(opsPerSec: number | null | undefined): string {
  if (opsPerSec === null || opsPerSec === undefined) return "—";
  if (opsPerSec >= 1_000_000) return `${(opsPerSec / 1_000_000).toFixed(2)} M ops/s`;
  if (opsPerSec >= 1_000) return `${(opsPerSec / 1_000).toFixed(1)} K ops/s`;
  return `${opsPerSec.toFixed(1)} ops/s`;
}

export function formatTime(iso: string | null | undefined): string {
  if (!iso) return "—";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString();
}

export function formatUptime(seconds: number): string {
  if (seconds < 60) return `${Math.round(seconds)} s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ${Math.round(seconds % 60)} s`;
  return `${Math.floor(seconds / 3600)} h ${Math.floor((seconds % 3600) / 60)} min`;
}

/** Rows -> CSV text (used for client-side export of the currently shown measurements). */
export function toCsv(rows: Record<string, string | number | null>[], columns: string[]): string {
  const esc = (v: string | number | null) => {
    const s = v === null ? "" : String(v);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  return [columns.join(","), ...rows.map((r) => columns.map((c) => esc(r[c] ?? null)).join(","))].join("\n");
}
