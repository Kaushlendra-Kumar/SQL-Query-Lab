import { useEffect, useRef } from 'react';
import Editor, { loader, type OnMount } from '@monaco-editor/react';
import * as monaco from 'monaco-editor/esm/vs/editor/editor.api';
import 'monaco-editor/esm/vs/basic-languages/sql/sql.contribution';
import EditorWorker from 'monaco-editor/esm/vs/editor/editor.worker?worker';
import type { ApiError, Catalog } from '../types/api';

// Bundle Monaco locally so the app works offline (no CDN).
(self as unknown as { MonacoEnvironment: unknown }).MonacoEnvironment = { getWorker: () => new EditorWorker() };
loader.config({ monaco });

let themed = false;
function defineTheme(m: typeof monaco) {
  if (themed) return;
  themed = true;
  m.editor.defineTheme('querylab', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'keyword', foreground: '5FB8E8' },
      { token: 'string', foreground: 'E9B44C' },
      { token: 'number', foreground: 'B49BEA' },
      { token: 'comment', foreground: '6B7891', fontStyle: 'italic' },
      { token: 'operator', foreground: 'DDE4EF' },
      { token: 'predefined', foreground: '63CDB0' },
      { token: 'predefined.sql', foreground: '63CDB0' },
      { token: 'delimiter', foreground: '8D9AB0' },
    ],
    colors: {
      'editor.background': '#121722',
      'editor.lineHighlightBackground': '#1A2130',
      'editorLineNumber.foreground': '#3E4A63',
      'editorLineNumber.activeForeground': '#8D9AB0',
      'editor.selectionBackground': '#2E5C8666',
      'editorCursor.foreground': '#DDE4EF',
      'editorError.foreground': '#EE7D6A',
    },
  });
}

interface Props {
  value: string;
  onChange: (v: string) => void;
  onRun: () => void;
  onExplain: () => void;
  error?: ApiError | null;
  /** Absolute offset of the failing statement, so error spans line up. */
  catalog: Catalog | null;
}

export function SqlEditor({ value, onChange, onRun, onExplain, error, catalog }: Props) {
  const editorRef = useRef<monaco.editor.IStandaloneCodeEditor | null>(null);
  const runRef = useRef(onRun);
  const explainRef = useRef(onExplain);
  runRef.current = onRun;
  explainRef.current = onExplain;
  const catalogRef = useRef(catalog);
  catalogRef.current = catalog;

  const onMount: OnMount = (editor, m) => {
    editorRef.current = editor;
    editor.addCommand(m.KeyMod.CtrlCmd | m.KeyCode.Enter, () => runRef.current());
    editor.addCommand(m.KeyMod.CtrlCmd | m.KeyMod.Shift | m.KeyCode.Enter, () => explainRef.current());
    // Table and column completions from the live catalog.
    m.languages.registerCompletionItemProvider('sql', {
      provideCompletionItems: (model, position) => {
        const word = model.getWordUntilPosition(position);
        const range = { startLineNumber: position.lineNumber, endLineNumber: position.lineNumber, startColumn: word.startColumn, endColumn: word.endColumn };
        const items: monaco.languages.CompletionItem[] = [];
        for (const t of catalogRef.current?.tables ?? []) {
          items.push({ label: t.name, kind: m.languages.CompletionItemKind.Struct, insertText: t.name, detail: `table, ${t.rowCount} rows`, range });
          for (const c of t.columns) {
            items.push({ label: c.name, kind: m.languages.CompletionItemKind.Field, insertText: c.name, detail: `${t.name}.${c.name} ${c.type}`, range });
          }
        }
        return { suggestions: items };
      },
    });
  };

  // Show the engine's error location as an editor marker.
  useEffect(() => {
    const editor = editorRef.current;
    const model = editor?.getModel();
    if (!editor || !model) return;
    if (error?.span) {
      const start = model.getPositionAt(error.span.begin);
      const end = model.getPositionAt(Math.max(error.span.end, error.span.begin + 1));
      monaco.editor.setModelMarkers(model, 'querylab', [
        {
          severity: monaco.MarkerSeverity.Error,
          message: error.display,
          startLineNumber: start.lineNumber,
          startColumn: start.column,
          endLineNumber: end.lineNumber,
          endColumn: end.column,
        },
      ]);
    } else {
      monaco.editor.setModelMarkers(model, 'querylab', []);
    }
  }, [error]);

  return (
    <Editor
      language="sql"
      theme="querylab"
      value={value}
      beforeMount={defineTheme}
      onMount={onMount}
      onChange={(v) => onChange(v ?? '')}
      options={{
        fontFamily: '"JetBrains Mono", monospace',
        fontSize: 13,
        lineHeight: 21,
        minimap: { enabled: false },
        scrollBeyondLastLine: false,
        padding: { top: 10, bottom: 10 },
        renderLineHighlight: 'line',
        tabSize: 2,
        wordWrap: 'on',
        automaticLayout: true,
        fixedOverflowWidgets: true,
        bracketPairColorization: { enabled: false },
      }}
    />
  );
}
