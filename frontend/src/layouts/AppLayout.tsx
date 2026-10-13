import { useState } from "react";
import { NavLink, Outlet } from "react-router-dom";
import { api } from "../services/api";
import { useAsync } from "../hooks/useAsync";
import { Badge } from "../components/ui";
import { formatUptime } from "../utils/format";
import { usePolling } from "../hooks/usePolling";
import type { Health } from "../types/api";

const NAV = [
  { to: "/", label: "Overview", icon: "M3 12l9-9 9 9M5 10v10h5v-6h4v6h5V10" },
  { to: "/query", label: "Query Workbench", icon: "M8 9l-4 3 4 3M16 9l4 3-4 3M13 6l-2 12" },
  { to: "/tree", label: "B+ Tree Explorer", icon: "M12 3v5M12 8l-6 6M12 8l6 6M4 14h4v5H4zM16 14h4v5h-4zM10 3h4v3h-4z" },
  { to: "/storage", label: "Storage Explorer", icon: "M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zM4 6v6c0 1.7 3.6 3 8 3s8-1.3 8-3V6M4 12v6c0 1.7 3.6 3 8 3s8-1.3 8-3v-6" },
  { to: "/performance", label: "Performance Lab", icon: "M4 20V10M10 20V4M16 20v-7M22 20H2" },
  { to: "/diagnostics", label: "Diagnostics", icon: "M12 9v4M12 17h.01M10.3 3.9L2.4 18a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z" },
];

function HealthPill({ health, error }: { health: Health | null; error: boolean }) {
  if (error || !health) return <Badge tone="red">API unreachable</Badge>;
  return health.status === "ok" ? <Badge tone="green">Engine running</Badge> : <Badge tone="red" title={health.error ?? ""}>Engine unavailable</Badge>;
}

export default function AppLayout() {
  const [open, setOpen] = useState(false);
  const health = usePolling(() => api.health(), 5000);
  const build = useAsync(() => api.buildInfo(), []);
  return (
    <div className="flex min-h-full">
      <aside
        className={`fixed inset-y-0 left-0 z-30 flex w-64 flex-col bg-navy-900 text-slate-300 transition-transform md:static md:translate-x-0 ${open ? "translate-x-0" : "-translate-x-full"}`}
      >
        <div className="flex items-center gap-3 border-b border-white/10 px-5 py-5">
          <img src="/favicon.svg" alt="" className="h-9 w-9" />
          <div>
            <div className="text-base font-semibold tracking-tight text-white">IndexCraft</div>
            <div className="text-xs text-slate-400">Storage engine workstation</div>
          </div>
        </div>
        <nav className="flex-1 space-y-1 px-3 py-4" aria-label="Main">
          {NAV.map((item) => (
            <NavLink
              key={item.to}
              to={item.to}
              end={item.to === "/"}
              onClick={() => setOpen(false)}
              className={({ isActive }) =>
                `flex items-center gap-3 rounded-lg px-3 py-2.5 text-sm font-medium transition-colors ${
                  isActive ? "bg-blue-500/15 text-white ring-1 ring-inset ring-blue-400/30" : "hover:bg-white/5 hover:text-white"
                }`
              }
            >
              <svg className="h-5 w-5 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round">
                <path d={item.icon} />
              </svg>
              {item.label}
            </NavLink>
          ))}
        </nav>
        <div className="border-t border-white/10 px-5 py-4 text-xs text-slate-400">
          <div>C++17 heap file + B+ Tree</div>
          <div className="mt-0.5">
            Engine {build.data?.engine ? `v${build.data.engine.version} (${build.data.engine.build_type})` : "—"}
          </div>
        </div>
      </aside>
      {open && <div className="fixed inset-0 z-20 bg-slate-900/40 md:hidden" onClick={() => setOpen(false)} />}

      <div className="flex min-w-0 flex-1 flex-col">
        <header className="sticky top-0 z-10 flex items-center justify-between gap-3 border-b border-slate-200 bg-white/90 px-4 py-3 backdrop-blur md:px-8">
          <button className="rounded-lg p-2 text-slate-700 hover:bg-slate-100 md:hidden" onClick={() => setOpen(true)} aria-label="Open navigation">
            <svg className="h-5 w-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M4 6h16M4 12h16M4 18h16" /></svg>
          </button>
          <div className="hidden text-sm text-slate-500 md:block">
            BCSE302P · A lightweight relational storage engine with adaptive B+ Tree indexing
          </div>
          <div className="flex items-center gap-3 text-xs text-slate-500">
            {health.data && <span className="hidden sm:inline">API up {formatUptime(health.data.uptime_seconds)}</span>}
            <HealthPill health={health.data} error={!!health.error} />
          </div>
        </header>
        <main className="mx-auto w-full max-w-7xl flex-1 px-4 py-6 md:px-8 md:py-8">
          {health.data && health.data.status === "degraded" && (
            <div role="alert" className="mb-6 rounded-lg border border-red-200 bg-red-50 px-4 py-3 text-sm text-red-900">
              <strong>The storage engine is not running.</strong> {health.data.error}
            </div>
          )}
          <Outlet />
        </main>
      </div>
    </div>
  );
}
