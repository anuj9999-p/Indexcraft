import { useEffect, useRef, useState } from "react";
import { ApiError } from "../services/api";

/** Calls `fn` immediately and then every `intervalMs` while the component is mounted. */
export function usePolling<T>(fn: () => Promise<T>, intervalMs: number, enabled = true): { data: T | null; error: ApiError | null } {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<ApiError | null>(null);
  const fnRef = useRef(fn);
  fnRef.current = fn;

  useEffect(() => {
    if (!enabled) return;
    let alive = true;
    const run = () =>
      fnRef
        .current()
        .then((d) => {
          if (alive) {
            setData(d);
            setError(null);
          }
        })
        .catch((e: unknown) => {
          if (alive) setError(e instanceof ApiError ? e : new ApiError(0, "ERROR", String(e)));
        });
    run();
    const id = setInterval(run, intervalMs);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, [intervalMs, enabled]);

  return { data, error };
}
