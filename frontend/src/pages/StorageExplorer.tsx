import { useState } from "react";
import { api, ApiError } from "../services/api";
import { useAsync } from "../hooks/useAsync";
import { useToast } from "../hooks/useToast";
import { Button, Card, ConfirmDialog, DataTable, EmptyState, ErrorBanner, Field, LoadingBlock, PageHeader, StatCard, inputClass } from "../components/ui";
import { formatBytes, formatInt } from "../utils/format";
import type { DbRecord } from "../types/api";

const PAGE_SIZE = 25;

export default function StorageExplorer() {
  const toast = useToast();
  const [page, setPage] = useState(0);
  const [version, setVersion] = useState(0);
  const list = useAsync(() => api.listRecords(page * PAGE_SIZE, PAGE_SIZE), [page, version]);
  const stats = useAsync(() => api.stats(), [version]);

  const [newKey, setNewKey] = useState("");
  const [newPayload, setNewPayload] = useState("");
  const [inserting, setInserting] = useState(false);

  const [searchKey, setSearchKey] = useState("");
  const [found, setFound] = useState<DbRecord | null>(null);
  const [searchMsg, setSearchMsg] = useState<string | null>(null);

  const [toDelete, setToDelete] = useState<number | null>(null);
  const [deleting, setDeleting] = useState(false);

  const refresh = () => setVersion((v) => v + 1);
  const fail = (e: unknown) => toast("error", e instanceof ApiError ? e.message : String(e));

  async function insert() {
    const key = Number(newKey);
    if (newKey.trim() === "" || !Number.isInteger(key)) {
      toast("error", "Key must be an integer.");
      return;
    }
    setInserting(true);
    try {
      const r = await api.insertRecord(key, newPayload);
      toast("success", `Inserted key ${r.key} (record id ${r.rid}, file offset ${r.offset}).`);
      setNewKey("");
      setNewPayload("");
      refresh();
    } catch (e) {
      fail(e);
    } finally {
      setInserting(false);
    }
  }

  async function search() {
    const key = Number(searchKey);
    if (searchKey.trim() === "" || !Number.isInteger(key)) {
      toast("error", "Search key must be an integer.");
      return;
    }
    try {
      setFound(await api.getRecord(key));
      setSearchMsg(null);
    } catch (e) {
      setFound(null);
      setSearchMsg(e instanceof ApiError ? e.message : String(e));
    }
  }

  async function confirmDelete() {
    if (toDelete === null) return;
    setDeleting(true);
    try {
      await api.deleteRecord(toDelete);
      toast("success", `Deleted key ${toDelete}. Its slot is now a tombstone and is reusable.`);
      if (found?.key === toDelete) setFound(null);
      setToDelete(null);
      refresh();
    } catch (e) {
      fail(e);
    } finally {
      setDeleting(false);
    }
  }

  const total = list.data?.total ?? 0;
  const pages = Math.max(1, Math.ceil(total / PAGE_SIZE));
  const s = stats.data;

  return (
    <>
      <PageHeader title="Storage Explorer" subtitle="Records stored in the binary heap file. Listing is ordered by key through the B+ Tree leaf chain." />
      <div className="space-y-6">
        <div className="grid grid-cols-2 gap-4 lg:grid-cols-4">
          <StatCard label="Live records" value={formatInt(s?.record_count)} />
          <StatCard label="File size" value={s ? formatBytes(s.file_size_bytes) : "—"} />
          <StatCard label="Deleted slots" value={formatInt(s?.deleted_slots)} hint="reusable tombstones" />
          <StatCard label="Next record id" value={formatInt(s?.next_record_id)} hint="never reused" />
        </div>

        <div className="grid gap-6 lg:grid-cols-2">
          <Card title="Insert record" subtitle="Keys are unique 64-bit integers (API limit ±2^53−1).">
            <form
              className="space-y-3"
              onSubmit={(e) => {
                e.preventDefault();
                void insert();
              }}
            >
              <Field label="Key">
                <input className={inputClass} value={newKey} onChange={(e) => setNewKey(e.target.value)} inputMode="numeric" placeholder="e.g. 42" />
              </Field>
              <Field label="Payload (text)">
                <input className={inputClass} value={newPayload} onChange={(e) => setNewPayload(e.target.value)} placeholder="record payload" />
              </Field>
              <Button type="submit" variant="primary" busy={inserting}>
                Insert
              </Button>
            </form>
          </Card>

          <Card title="Search by key" subtitle="Uses the B+ Tree to locate the record, then reads one heap slot.">
            <form
              className="flex items-end gap-2"
              onSubmit={(e) => {
                e.preventDefault();
                void search();
              }}
            >
              <div className="flex-1">
                <Field label="Key">
                  <input className={inputClass} value={searchKey} onChange={(e) => setSearchKey(e.target.value)} inputMode="numeric" />
                </Field>
              </div>
              <Button type="submit">Search</Button>
            </form>
            {found && (
              <div className="mt-3 rounded-lg border border-emerald-200 bg-emerald-50 p-3 text-sm">
                <div className="font-mono">key {found.key} · rid {found.rid}</div>
                <div className="mt-1 break-all text-slate-700">{found.payload}</div>
              </div>
            )}
            {searchMsg && <div className="mt-3 rounded-lg border border-amber-200 bg-amber-50 p-3 text-sm text-amber-900">{searchMsg}</div>}
          </Card>
        </div>

        <Card
          title="Records"
          actions={
            <div className="flex items-center gap-2 text-sm">
              <Button onClick={() => setPage((p) => Math.max(0, p - 1))} disabled={page === 0}>
                Prev
              </Button>
              <span className="tabular text-slate-600">
                Page {page + 1} / {pages}
              </span>
              <Button onClick={() => setPage((p) => Math.min(pages - 1, p + 1))} disabled={page >= pages - 1}>
                Next
              </Button>
            </div>
          }
        >
          {list.error && <ErrorBanner error={list.error} onRetry={list.reload} />}
          {!list.data && !list.error && <LoadingBlock />}
          {list.data && list.data.records.length === 0 && (
            <EmptyState title="No records">Insert one above, or generate samples from the Overview page.</EmptyState>
          )}
          {list.data && list.data.records.length > 0 && (
            <DataTable columns={[{ label: "Key", align: "right" }, { label: "Record id", align: "right" }, { label: "Payload" }, { label: "" }]}>
              {list.data.records.map((r) => (
                <tr key={r.rid}>
                  <td className="tabular px-3 py-2 text-right font-mono">{r.key}</td>
                  <td className="tabular px-3 py-2 text-right font-mono text-slate-500">{r.rid}</td>
                  <td className="max-w-md truncate px-3 py-2" title={r.payload}>
                    {r.payload}
                  </td>
                  <td className="px-3 py-2 text-right">
                    <Button variant="ghost" onClick={() => setToDelete(r.key)}>
                      Delete
                    </Button>
                  </td>
                </tr>
              ))}
            </DataTable>
          )}
        </Card>
      </div>
      <ConfirmDialog open={toDelete !== null} title="Delete record?" confirmLabel="Delete" busy={deleting} onConfirm={() => void confirmDelete()} onCancel={() => setToDelete(null)}>
        Key {toDelete} will be removed from the index and its heap slot marked deleted. This cannot be undone.
      </ConfirmDialog>
    </>
  );
}
