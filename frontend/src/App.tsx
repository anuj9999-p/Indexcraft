import { Navigate, Route, Routes } from "react-router-dom";
import AppLayout from "./layouts/AppLayout";
import Overview from "./pages/Overview";
import QueryWorkbench from "./pages/QueryWorkbench";
import TreeExplorer from "./pages/TreeExplorer";
import StorageExplorer from "./pages/StorageExplorer";
import PerformanceLab from "./pages/PerformanceLab";
import Diagnostics from "./pages/Diagnostics";

export default function App() {
  return (
    <Routes>
      <Route element={<AppLayout />}>
        <Route index element={<Overview />} />
        <Route path="query" element={<QueryWorkbench />} />
        <Route path="tree" element={<TreeExplorer />} />
        <Route path="storage" element={<StorageExplorer />} />
        <Route path="performance" element={<PerformanceLab />} />
        <Route path="diagnostics" element={<Diagnostics />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Route>
    </Routes>
  );
}
