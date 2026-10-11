import { useEffect } from 'react';
import { useStore } from './store';
import { Sidebar, TopBar } from './components/Shell';
import { EditorPage } from './pages/EditorPage';
import { ExplainPage } from './pages/ExplainPage';
import { OptimizerPage } from './pages/OptimizerPage';
import { StoragePage } from './pages/StoragePage';
import { LabPage } from './pages/LabPage';

export default function App() {
  const page = useStore((s) => s.page);
  const notice = useStore((s) => s.notice);
  const setNotice = useStore((s) => s.setNotice);
  const refreshCatalog = useStore((s) => s.refreshCatalog);
  const loadDatasets = useStore((s) => s.loadDatasets);

  useEffect(() => {
    void refreshCatalog();
    void loadDatasets();
  }, [refreshCatalog, loadDatasets]);

  useEffect(() => {
    if (!notice) return;
    const t = window.setTimeout(() => setNotice(null), 5000);
    return () => window.clearTimeout(t);
  }, [notice, setNotice]);

  return (
    <div className="h-full flex flex-col">
      <TopBar />
      <div className="flex-1 min-h-0 flex">
        <Sidebar />
        <main className="flex-1 min-w-0 min-h-0">
          {page === 'editor' && <EditorPage />}
          {page === 'explain' && <ExplainPage />}
          {page === 'optimizer' && <OptimizerPage />}
          {page === 'storage' && <StoragePage />}
          {page === 'lab' && <LabPage />}
        </main>
      </div>
      {notice && (
        <div className="fixed bottom-4 left-1/2 -translate-x-1/2 panel px-4 py-2 text-sm shadow-xl" role="status">
          {notice}
        </div>
      )}
    </div>
  );
}
