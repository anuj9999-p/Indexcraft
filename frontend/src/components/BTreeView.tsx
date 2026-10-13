import { useEffect, useMemo, useRef, useState } from "react";
import type { TreeData, TreeNode } from "../types/api";

const NODE_H = 36;
const LEVEL_GAP = 92;
const H_GAP = 16;
const PAD_X = 10;
const LEFT_MARGIN = 96;
const TOP_MARGIN = 26;
const MAX_SHOWN_KEYS = 10;

interface Laid {
  node: TreeNode;
  x: number; // centre
  y: number; // top
  w: number;
  cells: (number | "…")[];
}

/** Keys to draw inside a node: all of them when few, otherwise head, the searched key, and tail. */
function visibleKeys(n: TreeNode, focus: number | null): (number | "…")[] {
  if (n.keys.length <= MAX_SHOWN_KEYS) return n.keys;
  const head = n.keys.slice(0, 3);
  const tail = n.keys.slice(-3);
  const mid = focus !== null && n.keys.includes(focus) && !head.includes(focus) && !tail.includes(focus) ? [focus] : [];
  return [...head, "…", ...mid, ...(mid.length ? (["…"] as const) : []), ...tail];
}

function cellWidth(v: number | "…"): number {
  return v === "…" ? 18 : Math.max(26, String(v).length * 8 + 12);
}

export interface BTreeViewProps {
  tree: TreeData;
  pathIds: number[];
  foundKey: number | null;
  selectedId: number | null;
  onSelect: (id: number) => void;
}

export default function BTreeView({ tree, pathIds, foundKey, selectedId, onSelect }: BTreeViewProps) {
  const layout = useMemo(() => {
    const byId = new Map<number, TreeNode>(tree.nodes.map((n) => [n.id, n]));
    const laid = new Map<number, Laid>();
    let cursor = LEFT_MARGIN;
    const place = (id: number): Laid | null => {
      const node = byId.get(id);
      if (!node) return null;
      const cells = visibleKeys(node, foundKey);
      const w = Math.max(56, cells.reduce<number>((s, c) => s + cellWidth(c), 0) + PAD_X * 2);
      const kids = node.leaf ? [] : node.children.map(place).filter((k): k is Laid => k !== null);
      let x: number;
      if (kids.length) {
        x = (kids[0].x + kids[kids.length - 1].x) / 2;
      } else {
        x = cursor + w / 2;
        cursor += w + H_GAP;
      }
      const l: Laid = { node, x, y: TOP_MARGIN + node.level * LEVEL_GAP, w, cells };
      laid.set(id, l);
      return l;
    };
    place(tree.root_id);
    const width = Math.max(cursor + 20, 400);
    const height = TOP_MARGIN + tree.height * LEVEL_GAP + 10;
    return { laid, width, height };
  }, [tree, foundKey]);

  const scroller = useRef<HTMLDivElement>(null);
  const [fit, setFit] = useState(false);
  const focusId = pathIds.length ? pathIds[pathIds.length - 1] : tree.root_id;
  useEffect(() => {
    // Centre the viewport on the root (or on the last node of a traced search path).
    const el = scroller.current;
    const target = layout.laid.get(focusId);
    if (el && target && !fit) el.scrollLeft = Math.max(0, target.x - el.clientWidth / 2);
  }, [layout, focusId, fit]);

  const pathSet = new Set(pathIds);
  const pathEdges = new Set(pathIds.slice(1).map((id, i) => `${pathIds[i]}>${id}`));
  const levels = Array.from({ length: tree.height }, (_, i) => i);

  return (
    <div>
    <div className="mb-2 flex justify-end">
      <button type="button" onClick={() => setFit((f) => !f)} className="rounded border border-slate-300 bg-white px-2 py-1 text-xs text-slate-700 hover:bg-slate-50" aria-pressed={fit}>
        {fit ? "Actual size (scroll)" : "Fit whole tree to width"}
      </button>
    </div>
    <div ref={scroller} className="overflow-auto rounded-lg border border-slate-200 bg-white">
      <svg width={fit ? "100%" : layout.width} height={fit ? undefined : layout.height} viewBox={`0 0 ${layout.width} ${layout.height}`} role="img" aria-label="B+ Tree structure">
        <defs>
          <marker id="leafArrow" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto">
            <path d="M0 0 L8 4 L0 8 z" fill="#94a3b8" />
          </marker>
        </defs>

        {levels.map((lv) => (
          <text key={lv} x={8} y={TOP_MARGIN + lv * LEVEL_GAP + NODE_H / 2 + 4} fontSize="11" fill="#64748b">
            {lv === 0 ? "root" : lv === tree.height - 1 ? "leaves" : `level ${lv}`}
          </text>
        ))}

        {/* parent -> child edges */}
        {[...layout.laid.values()].flatMap((p) =>
          p.node.leaf
            ? []
            : p.node.children.map((cid) => {
                const c = layout.laid.get(cid);
                if (!c) return null;
                const on = pathEdges.has(`${p.node.id}>${cid}`);
                const x1 = p.x, y1 = p.y + NODE_H, x2 = c.x, y2 = c.y, my = (y1 + y2) / 2;
                return (
                  <path key={`${p.node.id}-${cid}`} d={`M${x1} ${y1} C${x1} ${my}, ${x2} ${my}, ${x2} ${y2}`} fill="none" stroke={on ? "#2563eb" : "#cbd5e1"} strokeWidth={on ? 3 : 1.5} />
                );
              }),
        )}

        {/* leaf sibling links */}
        {[...layout.laid.values()]
          .filter((l) => l.node.leaf && l.node.next !== null)
          .map((l) => {
            const nx = layout.laid.get(l.node.next as number);
            if (!nx) return null;
            const y = l.y + NODE_H / 2;
            return <line key={`n-${l.node.id}`} x1={l.x + l.w / 2} y1={y} x2={nx.x - nx.w / 2 - 1} y2={y} stroke="#94a3b8" strokeWidth={1.5} strokeDasharray="4 3" markerEnd="url(#leafArrow)" />;
          })}

        {/* nodes */}
        {[...layout.laid.values()].map((l) => {
          const onPath = pathSet.has(l.node.id);
          const selected = selectedId === l.node.id;
          let cx = l.x - l.w / 2 + PAD_X;
          return (
            <g key={l.node.id} onClick={() => onSelect(l.node.id)} style={{ cursor: "pointer" }} role="button" aria-label={`${l.node.leaf ? "Leaf" : "Internal"} node ${l.node.id}, ${l.node.keys.length} keys`}>
              <rect
                x={l.x - l.w / 2}
                y={l.y}
                width={l.w}
                height={NODE_H}
                rx={6}
                fill={l.node.leaf ? "#ffffff" : "#eff6ff"}
                stroke={selected ? "#0f172a" : onPath ? "#2563eb" : l.node.leaf ? "#94a3b8" : "#60a5fa"}
                strokeWidth={onPath || selected ? 2.5 : 1.25}
              />
              {l.cells.map((c, i) => {
                const w = cellWidth(c);
                const x = cx;
                cx += w;
                const hit = c !== "…" && foundKey !== null && c === foundKey && l.node.leaf;
                return (
                  <g key={i}>
                    {hit && <rect x={x + 1} y={l.y + 5} width={w - 2} height={NODE_H - 10} rx={4} fill="#2563eb" />}
                    <text x={x + w / 2} y={l.y + NODE_H / 2 + 4} textAnchor="middle" fontSize="12" fontFamily="ui-monospace, monospace" fill={hit ? "#fff" : "#0f172a"}>
                      {c}
                    </text>
                  </g>
                );
              })}
              {/* occupancy bar */}
              <rect x={l.x - l.w / 2 + 2} y={l.y + NODE_H + 3} width={l.w - 4} height={3} rx={1.5} fill="#e2e8f0" />
              <rect x={l.x - l.w / 2 + 2} y={l.y + NODE_H + 3} width={(l.w - 4) * Math.min(1, l.node.occupancy)} height={3} rx={1.5} fill={l.node.occupancy < 0.5 ? "#f59e0b" : "#3b82f6"} />
            </g>
          );
        })}
      </svg>
    </div>
    </div>
  );
}
